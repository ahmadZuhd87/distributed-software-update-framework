#include "common.h"
#include "logger.h"
#include "config.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ---- Client configuration ---------------------------------------------- */

typedef struct {
    char host[128];                 /* server host / IP                     */
    int  port;                      /* server port                          */
    int  current_version;           /* installed software version           */
    char download_dir[256];         /* where downloaded files are stored    */
    char client_name[64];           /* label used in logs                   */
} client_cfg_t;

static client_cfg_t g_cfg;

/* ---- getCurrentVersion(): as named in the spec ------------------------- */
/* Returns the version of the software currently "installed" on this client. */
int getCurrentVersion(void)
{
    return g_cfg.current_version;
}

/* ---- Connect to the server --------------------------------------------- */
/* Resolves host (name or dotted-quad) and returns a connected socket fd,
 * or -1 on failure. */
static int connect_to_server(const char *host, int port, const char *cli)
{
    struct addrinfo hints, *res = NULL, *rp;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0) {
        log_event(LOG_ERR, cli, "cannot resolve host '%s': %s",
                  host, gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break; /* success */
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0)
        log_event(LOG_ERR, cli, "connection to %s:%d failed: %s",
                  host, port, strerror(errno));
    return fd;
}

/* ---- Receive exactly `size` bytes and write them to `path` ------------- */
static int download_to_file(int fd, const char *path, long size, const char *cli)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        log_event(LOG_ERR, cli, "cannot create local file '%s': %s",
                  path, strerror(errno));
        return -1;
    }

    char buf[XFER_CHUNK];
    long remaining = size;

    while (remaining > 0) {
        size_t want = (remaining < (long)sizeof(buf)) ? (size_t)remaining
                                                      : sizeof(buf);
        ssize_t got = read(fd, buf, want);
        if (got < 0) {
            if (errno == EINTR) continue;
            log_event(LOG_ERR, cli, "read error during download: %s",
                      strerror(errno));
            fclose(fp);
            return -1;
        }
        if (got == 0) {
            log_event(LOG_ERR, cli,
                      "connection closed early: %ld of %ld bytes received",
                      size - remaining, size);
            fclose(fp);
            return -1;
        }
        if (fwrite(buf, 1, (size_t)got, fp) != (size_t)got) {
            log_event(LOG_ERR, cli, "local write error");
            fclose(fp);
            return -1;
        }
        remaining -= got;
    }

    fclose(fp);
    return 0;
}

/* ---- CheckForUpdate(): the heart of the client ------------------------- */
/* Returns 0 if already up to date, 1 if an update was downloaded & verified,
 * negative on error. */
int CheckForUpdate(void)
{
    const char *cli = g_cfg.client_name;

    /* 1) Connect. */
    int fd = connect_to_server(g_cfg.host, g_cfg.port, cli);
    if (fd < 0) return -1;
    log_event(LOG_INFO, cli, "connected to %s:%d", g_cfg.host, g_cfg.port);

    /* 2) Report current version. */
    int ver = getCurrentVersion();
    if (send_line(fd, "%s %d", MSG_VERSION, ver) != 0) {
        log_event(LOG_ERR, cli, "failed to send version");
        close(fd);
        return -1;
    }
    log_event(LOG_INFO, cli, "sent current version v%d", ver);

    /* 3) Wait for and parse the server's decision line. */
    char line[MAX_LINE];
    ssize_t r = read_line(fd, line, sizeof(line));
    if (r <= 0) {
        log_event(LOG_ERR, cli, "no response from server");
        close(fd);
        return -1;
    }

    /* 4a) UP TO DATE. */
    if (strncmp(line, MSG_UPTODATE, strlen(MSG_UPTODATE)) == 0) {
        printf("[%s] Your software (v%d) is already up to date.\n", cli, ver);
        log_event(LOG_INFO, cli, "server says: UP-TO-DATE");
        close(fd);
        return 0;
    }

    /* 4b) ERROR. */
    if (strncmp(line, MSG_ERROR, strlen(MSG_ERROR)) == 0) {
        printf("[%s] Server error: %s\n", cli, line + strlen(MSG_ERROR) + 1);
        log_event(LOG_WARN, cli, "server returned error: %s", line);
        close(fd);
        return -1;
    }

    /* 4c) UPDATE <ver> <filename> <size> <sha256> */
    if (strncmp(line, MSG_UPDATE, strlen(MSG_UPDATE)) == 0) {
        int  newver = 0;
        long size   = 0;
        char fname[MAX_FNAME]  = {0};
        char want_hex[SHA256_HEX_LEN] = {0};

        if (sscanf(line + strlen(MSG_UPDATE), "%d %255s %ld %64s",
                   &newver, fname, &size, want_hex) != 4) {
            log_event(LOG_ERR, cli, "malformed UPDATE header: '%s'", line);
            close(fd);
            return -1;
        }

        log_event(LOG_INFO, cli,
                  "update available: v%d -> v%d, file '%s' (%ld bytes)",
                  ver, newver, fname, size);
        printf("[%s] Update available (v%d -> v%d). Downloading '%s' (%ld bytes)...\n",
               cli, ver, newver, fname, size);

        /* Build the local destination path. We prefix the client name to keep
         * concurrent test clients from clobbering each other's downloads. */
        char dest[512];
        snprintf(dest, sizeof(dest), "%s/%s_%s",
                 g_cfg.download_dir, cli, fname);

        /* 5) Download + store locally. */
        if (download_to_file(fd, dest, size, cli) != 0) {
            log_event(LOG_ERR, cli, "failed download");
            close(fd);
            return -1;
        }
        log_event(LOG_INFO, cli, "stored update locally at '%s'", dest);

        /* 6) Verify integrity (checksum validation). */
        char got_hex[SHA256_HEX_LEN];
        if (sha256_file(dest, got_hex) == 0 &&
            strcmp(got_hex, want_hex) == 0) {
            log_event(LOG_INFO, cli, "checksum OK (sha256 verified)");
            printf("[%s] Checksum verified successfully.\n", cli);
        } else {
            log_event(LOG_ERR, cli,
                      "CHECKSUM MISMATCH! expected %s got %s", want_hex, got_hex);
            printf("[%s] WARNING: checksum mismatch, update may be corrupt.\n", cli);
            close(fd);
            return -1;
        }

        /* 7) Simulate executing/installing the update (spec allows simulation). */
        printf("[%s] Simulating installation of update v%d... done.\n",
               cli, newver);
        log_event(LOG_INFO, cli, "update v%d installed (simulated)", newver);

        close(fd);
        return 1;
    }

    /* Anything else is unexpected. */
    log_event(LOG_WARN, cli, "unexpected server reply: '%s'", line);
    close(fd);
    return -1;
}

/* ---- Configuration loading --------------------------------------------- */

static int load_config(const char *path, int version_override)
{
    config_t cfg;
    if (config_load(path, &cfg) != 0) return -1;

    strncpy(g_cfg.host, config_get_str(&cfg, "server_host", "127.0.0.1"),
            sizeof(g_cfg.host) - 1);
    g_cfg.port            = config_get_int(&cfg, "port", 9090);
    g_cfg.current_version = (version_override >= 0)
                          ? version_override
                          : config_get_int(&cfg, "current_version", 1);
    strncpy(g_cfg.download_dir,
            config_get_str(&cfg, "download_dir", "./client_storage"),
            sizeof(g_cfg.download_dir) - 1);
    strncpy(g_cfg.client_name,
            config_get_str(&cfg, "client_name", "client"),
            sizeof(g_cfg.client_name) - 1);
    return 0;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <config-file> [version-override] [client-name]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    int version_override = (argc >= 3) ? atoi(argv[2]) : -1;

    if (load_config(argv[1], version_override) != 0) {
        fprintf(stderr, "client: failed to load config '%s'\n", argv[1]);
        return EXIT_FAILURE;
    }

    /* Optional client-name override (argv[3]) so many clients can run at once
     * with distinct names/log identities for the concurrency tests. */
    if (argc >= 4) {
        strncpy(g_cfg.client_name, argv[3], sizeof(g_cfg.client_name) - 1);
        g_cfg.client_name[sizeof(g_cfg.client_name) - 1] = '\0';
    }

    config_t lcfg;
    config_load(argv[1], &lcfg);
    const char *log_path = config_get_str(&lcfg, "client_log_file",
                                          "logs/client.log");
    if (log_init(log_path) != 0)
        return EXIT_FAILURE;

    /* Ensure the download directory exists. */
    mkdir(g_cfg.download_dir, 0755);

    log_event(LOG_INFO, g_cfg.client_name,
              "client launched (target %s:%d, current v%d)",
              g_cfg.host, g_cfg.port, g_cfg.current_version);

    int rc = CheckForUpdate();

    log_event(LOG_INFO, g_cfg.client_name, "CheckForUpdate() returned %d", rc);
    log_close();

    /* Exit code: 0 = up to date, 2 = updated, 1 = error (so scripts can tell). */
    if (rc == 0) return 0;
    if (rc == 1) return 2;
    return 1;
}
