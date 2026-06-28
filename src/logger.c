/* ===========================================================================
 * logger.c  -  Thread-safe logging implementation.
 * ===========================================================================
 */

#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

static FILE           *g_fp   = NULL;             /* log file handle        */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *level_str(log_level_t lvl)
{
    switch (lvl) {
        case LOG_INFO: return "INFO";
        case LOG_WARN: return "WARN";
        case LOG_ERR:  return "ERROR";
        default:       return "?";
    }
}

int log_init(const char *path)
{
    pthread_mutex_lock(&g_lock);
    g_fp = fopen(path, "a");
    pthread_mutex_unlock(&g_lock);
    if (!g_fp) {
        fprintf(stderr, "logger: cannot open log file '%s'\n", path);
        return -1;
    }
    return 0;
}

void log_event(log_level_t level, const char *client, const char *fmt, ...)
{
    /* Build the timestamp: YYYY-MM-DD HH:MM:SS with millisecond resolution. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);

    char when[40];
    int  off = (int)strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);
    snprintf(when + off, sizeof(when) - off, ".%03ld", ts.tv_nsec / 1000000L);

    /* Thread id: the calling thread's id, rendered as an unsigned value so it
     * is stable and readable across the whole run. Process id is included too
     * so the advanced fork-based server's children are distinguishable. */
    unsigned long tid = (unsigned long)pthread_self();
    long          pid = (long)getpid();

    /* Render the variadic event description into a local buffer. */
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (!client) client = "-";

    /* Serialise the actual write so concurrent threads never interleave. */
    pthread_mutex_lock(&g_lock);
    if (g_fp) {
        fprintf(g_fp, "[%s] [%-5s] [pid=%ld tid=%lu] [%s] %s\n",
                when, level_str(level), pid, tid, client, msg);
        fflush(g_fp);
    }
    /* Also echo to console for live visibility during testing. */
    fprintf(stdout, "[%s] [%-5s] [pid=%ld tid=%lu] [%s] %s\n",
            when, level_str(level), pid, tid, client, msg);
    fflush(stdout);
    pthread_mutex_unlock(&g_lock);
}

void log_close(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_fp) {
        fflush(g_fp);
        fclose(g_fp);
        g_fp = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}
