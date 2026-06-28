/* ===========================================================================
 * netutil.c
 * ---------------------------------------------------------------------------
 * Robust socket I/O helpers shared by client and server.
 *
 * TCP is a byte stream: a single read()/write() may move fewer bytes than
 * requested. These wrappers loop until the full request is satisfied (or a
 * real error / EOF occurs) so the rest of the code can treat messages
 * atomically. EINTR (interrupted by a signal) is handled by retrying.
 * ===========================================================================
 */

#include "common.h"

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* Read exactly n bytes (or until EOF/error). */
ssize_t read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    char  *p   = (char *)buf;

    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;   /* retry on signal interruption */
            return -1;
        }
        if (r == 0) {                       /* peer closed the connection   */
            return (got == 0) ? 0 : (ssize_t)got;
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* Write exactly n bytes (or error). */
ssize_t write_full(int fd, const void *buf, size_t n)
{
    size_t      sent = 0;
    const char *p    = (const char *)buf;

    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return (ssize_t)sent;
}

/* Read one newline-terminated line. Newline is stripped; result NUL-terminated.
 * Reads one byte at a time which is perfectly fine for short control lines and
 * keeps us from accidentally consuming the binary payload that follows. */
ssize_t read_line(int fd, char *buf, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return -1;

    while (i < cap - 1) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {                       /* EOF */
            if (i == 0) return 0;
            break;
        }
        if (c == '\n') break;               /* end of line */
        if (c == '\r') continue;            /* tolerate CRLF */
        buf[i++] = c;
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

/* printf-style control line sender. */
int send_line(int fd, const char *fmt, ...)
{
    char    line[MAX_LINE];
    va_list ap;
    int     len;

    va_start(ap, fmt);
    len = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);

    if (len < 0) return -1;
    if (len > (int)sizeof(line) - 2) len = (int)sizeof(line) - 2;

    line[len++] = '\n';                     /* append the line terminator */
    return (write_full(fd, line, (size_t)len) == len) ? 0 : -1;
}
