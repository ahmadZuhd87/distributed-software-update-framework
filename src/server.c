#include "common.h"
#include "logger.h"
#include "config.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---- Global server state ------------------------------------------------ */

typedef struct {
    int   port;                       /* listening port                     */
    int   latest_version;             /* newest available software version  */
    char  repo_dir[256];              /* directory holding update packages  */
    char  update_file[MAX_FNAME];     /* update package file name           */
    int   backlog;                    /* listen() backlog                   */
} server_cfg_t;

static server_cfg_t   g_cfg;
static int            g_listen_fd = -1;
static volatile sig_atomic_t g_running = 1;

/* Simple statistics, protected by a mutex. */
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_total_connections = 0;
static long g_active_clients    = 0;
static long g_updates_sent      = 0;
static long g_uptodate_replies  = 0;

/* Per-thread argument bundle. */
typedef struct {
    int                 fd;
    struct sockaddr_in  addr;
} client_arg_t;

/* ---- Signal handling ---------------------------------------------------- */

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
    /* Shut down the listening socket so accept() returns and the main loop
     * can exit cleanly. Using shutdown()/close() from a handler is safe here
     * because we only touch a single fd and a sig_atomic_t flag. */
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

/* ---- Helpers ------------------------------------------------------------ */

/* Build the full path to the update package: repo_dir/update_file. */
static void build_update_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", g_cfg.repo_dir, g_cfg.update_file);
}

/* Stream the update file over `fd`. Returns 0 on success, -1 on error.
 * Each thread opens its own FILE* so concurrent transfers are independent. */
static int stream_file(int fd, const char *path, const char *cli)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        log_event(LOG_ERR, cli, "cannot open update file '%s': %s",
                  path, strerror(errno));
        return -1;
    }

    char    buf[XFER_CHUNK];
    size_t  n;
    long    total = 0;

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (write_full(fd, buf, n) != (ssize_t)n) {
            log_event(LOG_ERR, cli, "transfer interrupted after %ld bytes", total);
            fclose(fp);
            return -1;
        }
        total += (long)n;
    }
    fclose(fp);
    log_event(LOG_INFO, cli, "transfer completed: %ld bytes sent", total);
    return 0;
}

/* ---- Per-client worker thread ------------------------------------------- */

static void *client_thread(void *vp)
{
    client_arg_t *ca = (client_arg_t *)vp;
    int   fd = ca->fd;
    char  cli[64];

    /* Render "ip:port" once for use in every log line. */
    snprintf(cli, sizeof(cli), "%s:%d",
             inet_ntoa(ca->addr.sin_addr), ntohs(ca->addr.sin_port));
    free(ca);

    pthread_mutex_lock(&g_stats_lock);
    g_total_connections++;
    g_active_clients++;
    long active_now = g_active_clients;
    pthread_mutex_unlock(&g_stats_lock);

    log_event(LOG_INFO, cli, "connection accepted (active clients: %ld)", active_now);

    /* --- Step 1: receive the client's version line. --- */
    char line[MAX_LINE];
    ssize_t r = read_line(fd, line, sizeof(line));
    if (r <= 0) {
        log_event(LOG_WARN, cli, "client disconnected before sending version");
        goto done;
    }

    /* Expected: "VERSION <int>" */
    int client_version = -1;
    if (strncmp(line, MSG_VERSION, strlen(MSG_VERSION)) != 0 ||
        sscanf(line + strlen(MSG_VERSION), "%d", &client_version) != 1) {
        log_event(LOG_WARN, cli, "invalid request received: '%s'", line);
        send_line(fd, "%s malformed version request", MSG_ERROR);
        goto done;
    }

    log_event(LOG_INFO, cli, "version request: client reports v%d (latest v%d)",
              client_version, g_cfg.latest_version);

    /* --- Step 2: decide. --- */
    if (client_version >= g_cfg.latest_version) {
        send_line(fd, "%s", MSG_UPTODATE);
        log_event(LOG_INFO, cli, "update decision: UP-TO-DATE");
        pthread_mutex_lock(&g_stats_lock);
        g_uptodate_replies++;
        pthread_mutex_unlock(&g_stats_lock);
        goto done;
    }

    /* --- Step 3: an update is required. Prepare metadata. --- */
    char path[512];
    build_update_path(path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0) {
        log_event(LOG_ERR, cli, "update package missing: '%s'", path);
        send_line(fd, "%s update package unavailable on server", MSG_ERROR);
        goto done;
    }

    char hex[SHA256_HEX_LEN];
    if (sha256_file(path, hex) != 0) {
        log_event(LOG_ERR, cli, "failed to hash update package");
        send_line(fd, "%s internal checksum error", MSG_ERROR);
        goto done;
    }

    /* UPDATE <latest_version> <filename> <filesize> <sha256> */
    send_line(fd, "%s %d %s %ld %s",
              MSG_UPDATE, g_cfg.latest_version, g_cfg.update_file,
              (long)st.st_size, hex);
    log_event(LOG_INFO, cli,
              "update decision: UPDATE required -> sending '%s' (%ld bytes)",
              g_cfg.update_file, (long)st.st_size);

    /* --- Step 4: stream the payload. --- */
    if (stream_file(fd, path, cli) == 0) {
        pthread_mutex_lock(&g_stats_lock);
        g_updates_sent++;
        pthread_mutex_unlock(&g_stats_lock);
    } else {
        log_event(LOG_ERR, cli, "failed download to client");
    }

done:
    close(fd);
    pthread_mutex_lock(&g_stats_lock);
    g_active_clients--;
    long remaining = g_active_clients;
    pthread_mutex_unlock(&g_stats_lock);
    log_event(LOG_INFO, cli, "disconnected (active clients: %ld)", remaining);
    return NULL;
}

/* ---- Configuration loading ---------------------------------------------- */

static int load_config(const char *path)
{
    config_t cfg;
    if (config_load(path, &cfg) != 0) return -1;

    g_cfg.port           = config_get_int(&cfg, "port", 9090);
    g_cfg.latest_version = config_get_int(&cfg, "latest_version", 1);
    g_cfg.backlog        = config_get_int(&cfg, "backlog", 16);
    strncpy(g_cfg.repo_dir,
            config_get_str(&cfg, "repo_dir", "./server_repo"),
            sizeof(g_cfg.repo_dir) - 1);
    strncpy(g_cfg.update_file,
            config_get_str(&cfg, "update_file", "update.bin"),
            sizeof(g_cfg.update_file) - 1);
    return 0;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <config-file>\n", argv[0]);
        fprintf(stderr, "  (the config file supplies port, latest_version, "
                        "repo_dir, update_file, log_file, ...)\n");
        return EXIT_FAILURE;
    }

    if (load_config(argv[1]) != 0) {
        fprintf(stderr, "server: failed to load config '%s'\n", argv[1]);
        return EXIT_FAILURE;
    }

    /* Logger path also comes from the config (default under ./logs). */
    config_t lcfg;
    config_load(argv[1], &lcfg);
    const char *log_path = config_get_str(&lcfg, "log_file", "logs/server.log");
    if (log_init(log_path) != 0)
        return EXIT_FAILURE;

    /* Ignore SIGPIPE so a client vanishing mid-transfer cannot kill us;
     * write_full() will instead return an error we handle gracefully. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    log_event(LOG_INFO, "SERVER",
              "starting up: port=%d latest_version=%d repo='%s' file='%s'",
              g_cfg.port, g_cfg.latest_version, g_cfg.repo_dir, g_cfg.update_file);

    /* --- Create the listening socket. --- */
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        log_event(LOG_ERR, "SERVER", "socket() failed: %s", strerror(errno));
        log_close();
        return EXIT_FAILURE;
    }

    int yes = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family      = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    srv.sin_port        = htons((uint16_t)g_cfg.port);

    if (bind(g_listen_fd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        log_event(LOG_ERR, "SERVER", "bind() failed on port %d: %s",
                  g_cfg.port, strerror(errno));
        close(g_listen_fd);
        log_close();
        return EXIT_FAILURE;
    }

    if (listen(g_listen_fd, g_cfg.backlog) < 0) {
        log_event(LOG_ERR, "SERVER", "listen() failed: %s", strerror(errno));
        close(g_listen_fd);
        log_close();
        return EXIT_FAILURE;
    }

    log_event(LOG_INFO, "SERVER", "listening on port %d (backlog %d)",
              g_cfg.port, g_cfg.backlog);

    /* --- Accept loop: spawn a detached thread per client. --- */
    while (g_running) {
        struct sockaddr_in cliaddr;
        socklen_t          clilen = sizeof(cliaddr);

        int cfd = accept(g_listen_fd, (struct sockaddr *)&cliaddr, &clilen);
        if (cfd < 0) {
            if (!g_running) break;            /* shutting down */
            if (errno == EINTR) continue;
            log_event(LOG_WARN, "SERVER", "accept() failed: %s", strerror(errno));
            continue;
        }

        client_arg_t *ca = (client_arg_t *)malloc(sizeof(client_arg_t));
        if (!ca) {
            log_event(LOG_ERR, "SERVER", "out of memory; rejecting client");
            close(cfd);
            continue;
        }
        ca->fd   = cfd;
        ca->addr = cliaddr;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (pthread_create(&tid, &attr, client_thread, ca) != 0) {
            log_event(LOG_ERR, "SERVER", "pthread_create failed: %s",
                      strerror(errno));
            close(cfd);
            free(ca);
        }
        pthread_attr_destroy(&attr);
    }

    /* --- Shutdown. --- */
    if (g_listen_fd >= 0) close(g_listen_fd);

    pthread_mutex_lock(&g_stats_lock);
    log_event(LOG_INFO, "SERVER",
              "shutdown: total_connections=%ld updates_sent=%ld uptodate=%ld",
              g_total_connections, g_updates_sent, g_uptodate_replies);
    pthread_mutex_unlock(&g_stats_lock);

    log_event(LOG_INFO, "SERVER", "server stopped cleanly");
    log_close();
    return EXIT_SUCCESS;
}
