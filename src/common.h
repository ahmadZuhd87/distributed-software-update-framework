#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>    /* for ssize_t */

/* Maximum length of a single control line (generous; lines are tiny). */
#define MAX_LINE        1024

/* Maximum length we accept for a stored file name. */
#define MAX_FNAME       256

/* Size of the chunk buffer used while streaming the update file. */
#define XFER_CHUNK      8192

/* Protocol verb prefixes (kept as macros to avoid typos on both sides). */
#define MSG_VERSION     "VERSION"
#define MSG_UPTODATE    "UPTODATE"
#define MSG_UPDATE      "UPDATE"
#define MSG_ERROR       "ERROR"

/* --------------------------------------------------------------------------
 * Robust I/O helpers (implemented in netutil.c)
 * -------------------------------------------------------------------------- */

/* Read exactly `n` bytes into buf, looping over partial reads.
 * Returns n on success, 0 on clean EOF before any byte, -1 on error. */
ssize_t read_full(int fd, void *buf, size_t n);

/* Write exactly `n` bytes from buf, looping over partial writes.
 * Returns n on success, -1 on error. */
ssize_t write_full(int fd, const void *buf, size_t n);

/* Read a single '\n'-terminated line (newline stripped) into buf.
 * At most cap-1 bytes are stored and the result is NUL-terminated.
 * Returns the string length on success, 0 on EOF, -1 on error. */
ssize_t read_line(int fd, char *buf, size_t cap);

/* Send a printf-style formatted control line, appending '\n'.
 * Returns 0 on success, -1 on error. */
int send_line(int fd, const char *fmt, ...);

#endif /* COMMON_H */
