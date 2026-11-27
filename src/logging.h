#ifndef KOMODO_ALERTER_LOGGING_H
#define KOMODO_ALERTER_LOGGING_H

#include <stdarg.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define KOMODO_LOG_PRINTF_ATTR(fmt_idx, args_idx)                              \
    __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define KOMODO_LOG_PRINTF_ATTR(fmt_idx, args_idx)
#endif

typedef enum { LOG_LEVEL_INFO = 0, LOG_LEVEL_ERROR = 1 } log_level_t;

/**
 * Configure line-buffered output for stdout and stderr.
 */
static inline void log_init(void) {
    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    (void)setvbuf(stderr, NULL, _IOLBF, 0);
}

/**
 * Format and write a single log line using a va_list.
 */
KOMODO_LOG_PRINTF_ATTR(2, 0)
static inline void log_vwrite(log_level_t level, const char *fmt, va_list ap) {
    FILE *stream = (level == LOG_LEVEL_ERROR) ? stderr : stdout;
    const char *tag = (level == LOG_LEVEL_ERROR) ? "ERROR" : "INFO";

    char msg[1024];
    int written = vsnprintf(msg, sizeof(msg), fmt, ap);
    if (written < 0) {
        (void)fprintf(stream, "[%s] (log formatting error)\n", tag);
        return;
    }
    (void)fprintf(stream, "[%s] %s\n", tag, msg);
}

/**
 * Emit an informational log line to stdout.
 */
KOMODO_LOG_PRINTF_ATTR(1, 2)
static inline void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vwrite(LOG_LEVEL_INFO, fmt, ap);
    va_end(ap);
}

/**
 * Emit an error log line to stderr.
 */
KOMODO_LOG_PRINTF_ATTR(1, 2)
static inline void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vwrite(LOG_LEVEL_ERROR, fmt, ap);
    va_end(ap);
}

#endif /* KOMODO_ALERTER_LOGGING_H */
