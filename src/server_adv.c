#define _GNU_SOURCE
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
#include <sched.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---- Server configuration ----------------------------------------------- */

typedef struct {
    int   port;
    int   latest_version;
    char  repo_dir[256];
    char  update_file[MAX_FNAME];
    int   backlog;
} server_cfg_t;

static server_cfg_t g_cfg;
static int          g_listen_fd = -1;
static volatile sig_atomic_t g_running = 1;

/* ---- Shared statistics block (lives in System V shared memory) ---------- */

typedef struct {
    long total_connections;
    long active_clients;
    long updates_sent;
    long uptodate_replies;
    long errors;
} shared_stats_t;

static int             g_shmid = -1;     /* shared memory segment id   */
static int             g_semid = -1;     /* semaphore set id           */
static shared_stats_t *g_stats = NULL;   /* attached shared block      */

/* union required by some platforms for semctl(). */
union semun {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
};

/* ---- Semaphore P (lock) / V (unlock) operations ------------------------- */

static void sem_lock(void)
{
    struct sembuf op = { 0, -1, SEM_UNDO };   /* decrement -> wait/lock */
    while (semop(g_semid, &op, 1) == -1) {
        if (errno == EINTR) continue;
        break;
    }
}

static void sem_unlock(void)
{
    struct sembuf op = { 0, +1, SEM_UNDO };   /* increment -> signal/unlock */
    while (semop(g_semid, &op, 1) == -1) {
        if (errno == EINTR) continue;
        break;
    }
}

/* Create + attach the shared-memory stats block and its guarding semaphore. */
static int ipc_init(void)
{
    /* Shared memory: anonymous private key, sized to the stats struct. */
    g_shmid = shmget(IPC_PRIVATE, sizeof(shared_stats_t), IPC_CREAT | 0600);
    if (g_shmid < 0) { perror("shmget"); return -1; }

    g_stats = (shared_stats_t *)shmat(g_shmid, NULL, 0);
    if (g_stats == (void *)-1) { perror("shmat"); return -1; }
    memset(g_stats, 0, sizeof(*g_stats));

    /* Binary semaphore initialised to 1 (unlocked). */
    g_semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (g_semid < 0) { perror("semget"); return -1; }

    union semun arg;
    arg.val = 1;
    if (semctl(g_semid, 0, SETVAL, arg) < 0) { perror("semctl"); return -1; }

    return 0;
}

/* Detach + destroy the IPC objects on shutdown so nothing leaks in the
 * kernel's IPC tables (check with `ipcs`). */
static void ipc_destroy(void)
{
    if (g_stats && g_stats != (void *)-1) shmdt(g_stats);
    if (g_shmid >= 0) shmctl(g_shmid, IPC_RMID, NULL);
    if (g_semid >= 0) semctl(g_semid, 0, IPC_RMID);
}

/* ---- Signal handlers ---------------------------------------------------- */

/* Reap any finished children without blocking (prevents zombies). */
static void on_sigchld(int sig)
{
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) { /* keep reaping */ }
    errno = saved;
}

static void on_term(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

/* ---- Real-time scheduling ----------------------------------------------- */
/* Try to switch this process to SCHED_FIFO (a soft real-time policy) at a
 * mid-range priority. Requires CAP_SYS_NICE / root; if unavailable we log a
 * note and continue under the default policy so the server still runs. */
static void try_realtime(void)
{
    struct sched_param sp;
    int maxp = sched_get_priority_max(SCHED_FIFO);
    int minp = sched_get_priority_min(SCHED_FIFO);
    sp.sched_priority = minp + (maxp - minp) / 2;

    if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0) {
        log_event(LOG_INFO, "SERVER",
                  "real-time scheduling enabled (SCHED_FIFO, prio=%d)",
                  sp.sched_priority);
    } else {
        log_event(LOG_WARN, "SERVER",
                  "could not enable SCHED_FIFO (%s); using default scheduler "
                  "(run as root for real-time priority)", strerror(errno));
    }
}

/* ---- File transfer (runs in a dedicated thread inside each child) ------- */

typedef struct {
    int  fd;
    char path[512];
    char cli[64];
    int  ok;            /* set to 1 by the thread on success */
} xfer_arg_t;

static void build_update_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", g_cfg.repo_dir, g_cfg.update_file);
}

static void *xfer_thread(void *vp)
{
    xfer_arg_t *xa = (xfer_arg_t *)vp;
    xa->ok = 0;

    FILE *fp = fopen(xa->path, "rb");
    if (!fp) {
        log_event(LOG_ERR, xa->cli, "cannot open update file '%s': %s",
                  xa->path, strerror(errno));
        return NULL;
    }

    char   buf[XFER_CHUNK];
    size_t n;
    long   total = 0;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (write_full(xa->fd, buf, n) != (ssize_t)n) {
            log_event(LOG_ERR, xa->cli, "transfer interrupted after %ld bytes",
                      total);
            fclose(fp);
            return NULL;
        }
        total += (long)n;
    }
    fclose(fp);
    log_event(LOG_INFO, xa->cli, "transfer completed: %ld bytes sent (worker thread)",
              total);
    xa->ok = 1;
    return NULL;
}

/* ---- Per-client handler: runs in the FORKED CHILD PROCESS --------------- */

static void handle_client(int fd, struct sockaddr_in addr)
{
    char cli[64];
    snprintf(cli, sizeof(cli), "%s:%d",
             inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

    /* Update shared stats under the semaphore (cross-process mutual exclusion). */
    sem_lock();
    g_stats->total_connections++;
    g_stats->active_clients++;
    long active_now = g_stats->active_clients;
    sem_unlock();

    log_event(LOG_INFO, cli,
              "connection accepted [child pid=%d] (active clients: %ld)",
              (int)getpid(), active_now);

    /* 1) Receive version line. */
    char line[MAX_LINE];
    ssize_t r = read_line(fd, line, sizeof(line));
    if (r <= 0) {
        log_event(LOG_WARN, cli, "client disconnected before sending version");
        goto done;
    }

    int client_version = -1;
    if (strncmp(line, MSG_VERSION, strlen(MSG_VERSION)) != 0 ||
        sscanf(line + strlen(MSG_VERSION), "%d", &client_version) != 1) {
        log_event(LOG_WARN, cli, "invalid request received: '%s'", line);
        send_line(fd, "%s malformed version request", MSG_ERROR);
        sem_lock(); g_stats->errors++; sem_unlock();
        goto done;
    }

    log_event(LOG_INFO, cli, "version request: client reports v%d (latest v%d)",
              client_version, g_cfg.latest_version);

    /* 2) Decision. */
    if (client_version >= g_cfg.latest_version) {
        send_line(fd, "%s", MSG_UPTODATE);
        log_event(LOG_INFO, cli, "update decision: UP-TO-DATE");
        sem_lock(); g_stats->uptodate_replies++; sem_unlock();
        goto done;
    }

    /* 3) Prepare update metadata. */
    char path[512];
    build_update_path(path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0) {
        log_event(LOG_ERR, cli, "update package missing: '%s'", path);
        send_line(fd, "%s update package unavailable on server", MSG_ERROR);
        sem_lock(); g_stats->errors++; sem_unlock();
        goto done;
    }

    char hex[SHA256_HEX_LEN];
    if (sha256_file(path, hex) != 0) {
        log_event(LOG_ERR, cli, "failed to hash update package");
        send_line(fd, "%s internal checksum error", MSG_ERROR);
        sem_lock(); g_stats->errors++; sem_unlock();
        goto done;
    }

    send_line(fd, "%s %d %s %ld %s",
              MSG_UPDATE, g_cfg.latest_version, g_cfg.update_file,
              (long)st.st_size, hex);
    log_event(LOG_INFO, cli,
              "update decision: UPDATE required -> sending '%s' (%ld bytes)",
              g_cfg.update_file, (long)st.st_size);

    /* 4) Stream the payload in a dedicated WORKER THREAD (process + thread). */
    xfer_arg_t xa;
    xa.fd  = fd;
    xa.ok  = 0;
    strncpy(xa.path, path, sizeof(xa.path) - 1); xa.path[sizeof(xa.path)-1] = 0;
    strncpy(xa.cli,  cli,  sizeof(xa.cli)  - 1); xa.cli[sizeof(xa.cli)-1]  = 0;

    pthread_t tid;
    if (pthread_create(&tid, NULL, xfer_thread, &xa) != 0) {
        /* Fallback: do it inline if the thread cannot be created. */
        log_event(LOG_WARN, cli, "worker thread create failed; sending inline");
        xfer_thread(&xa);
    } else {
        pthread_join(tid, NULL);   /* wait for the transfer to finish */
    }

    if (xa.ok) {
        sem_lock(); g_stats->updates_sent++; sem_unlock();
    } else {
        log_event(LOG_ERR, cli, "failed download to client");
        sem_lock(); g_stats->errors++; sem_unlock();
    }

done:
    close(fd);
    sem_lock();
    g_stats->active_clients--;
    long remaining = g_stats->active_clients;
    sem_unlock();
    log_event(LOG_INFO, cli, "disconnected [child pid=%d] (active clients: %ld)",
              (int)getpid(), remaining);
}

/* ---- Config loading ----------------------------------------------------- */

static int load_config(const char *path)
{
    config_t cfg;
    if (config_load(path, &cfg) != 0) return -1;

    g_cfg.port           = config_get_int(&cfg, "port", 9090);
    g_cfg.latest_version = config_get_int(&cfg, "latest_version", 1);
    g_cfg.backlog        = config_get_int(&cfg, "backlog", 16);
    strncpy(g_cfg.repo_dir,    config_get_str(&cfg, "repo_dir", "server_repo"),
            sizeof(g_cfg.repo_dir) - 1);
    strncpy(g_cfg.update_file, config_get_str(&cfg, "update_file", "update.bin"),
            sizeof(g_cfg.update_file) - 1);
    return 0;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <config-file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (load_config(argv[1]) != 0) {
        fprintf(stderr, "server_adv: failed to load config '%s'\n", argv[1]);
        return EXIT_FAILURE;
    }

    config_t lcfg;
    config_load(argv[1], &lcfg);
    const char *log_path = config_get_str(&lcfg, "log_file", "logs/server.log");
    if (log_init(log_path) != 0) return EXIT_FAILURE;

    /* IPC: shared stats + guarding semaphore. */
    if (ipc_init() != 0) {
        log_event(LOG_ERR, "SERVER", "failed to initialise IPC (shm/sem)");
        log_close();
        return EXIT_FAILURE;
    }

    /* Signal wiring. */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = on_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_term);
    signal(SIGTERM, on_term);

    /* Try to gain real-time priority (degrades gracefully without root). */
    try_realtime();

    log_event(LOG_INFO, "SERVER",
              "ADVANCED server starting: port=%d latest_version=%d repo='%s' file='%s' "
              "[model: fork-per-client + worker thread, SysV shm/sem stats]",
              g_cfg.port, g_cfg.latest_version, g_cfg.repo_dir, g_cfg.update_file);

    /* Listening socket. */
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        log_event(LOG_ERR, "SERVER", "socket() failed: %s", strerror(errno));
        ipc_destroy(); log_close(); return EXIT_FAILURE;
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
        close(g_listen_fd); ipc_destroy(); log_close(); return EXIT_FAILURE;
    }
    if (listen(g_listen_fd, g_cfg.backlog) < 0) {
        log_event(LOG_ERR, "SERVER", "listen() failed: %s", strerror(errno));
        close(g_listen_fd); ipc_destroy(); log_close(); return EXIT_FAILURE;
    }

    log_event(LOG_INFO, "SERVER", "listening on port %d (backlog %d)",
              g_cfg.port, g_cfg.backlog);

    /* Accept loop: FORK a child process per client. */
    while (g_running) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);

        int cfd = accept(g_listen_fd, (struct sockaddr *)&cliaddr, &clilen);
        if (cfd < 0) {
            if (!g_running) break;
            if (errno == EINTR) continue;   /* interrupted by SIGCHLD etc. */
            log_event(LOG_WARN, "SERVER", "accept() failed: %s", strerror(errno));
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            log_event(LOG_ERR, "SERVER", "fork() failed: %s", strerror(errno));
            close(cfd);
            continue;
        }

        if (pid == 0) {
            /* ---- CHILD PROCESS ---- */
            close(g_listen_fd);          /* child doesn't accept new clients */
            handle_client(cfd, cliaddr);
            log_close();                 /* child closes its own log handle  */
            _exit(0);                    /* terminate child cleanly          */
        }

        /* ---- PARENT PROCESS ---- */
        close(cfd);                      /* parent doesn't talk to this client */
        /* SIGCHLD handler reaps the child when it exits (no zombies). */
    }

    /* Shutdown: wait for any remaining children, then dump shared stats. */
    if (g_listen_fd >= 0) close(g_listen_fd);
    while (waitpid(-1, NULL, WNOHANG) > 0) { }

    sem_lock();
    log_event(LOG_INFO, "SERVER",
              "shutdown: total_connections=%ld updates_sent=%ld uptodate=%ld errors=%ld",
              g_stats->total_connections, g_stats->updates_sent,
              g_stats->uptodate_replies, g_stats->errors);
    sem_unlock();

    log_event(LOG_INFO, "SERVER", "advanced server stopped cleanly");
    ipc_destroy();
    log_close();
    return EXIT_SUCCESS;
}
