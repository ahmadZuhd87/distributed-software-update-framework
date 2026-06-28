#ifndef LOGGER_H
#define LOGGER_H

/* Severity levels used to tag each entry. */
typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERR
} log_level_t;

/* Initialise the logger, opening `path` for appending.
 * Returns 0 on success, -1 on failure. */
int  log_init(const char *path);

/* Write a formatted entry.
 *   level  : severity
 *   client : short client descriptor ("ip:port", or "-" / "SERVER")
 *   fmt... : printf-style event description
 * The current thread id is captured automatically. Thread-safe. */
void log_event(log_level_t level, const char *client, const char *fmt, ...);

/* Flush and close the log file. */
void log_close(void);

#endif /* LOGGER_H */
