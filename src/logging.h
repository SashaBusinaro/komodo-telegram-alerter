#ifndef KOMODO_ALERTER_LOGGING_H
#define KOMODO_ALERTER_LOGGING_H

#include <stdarg.h>
#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define KOMODO_LOG_PRINTF_ATTR(fmt_idx, args_idx)                              \
    __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define KOMODO_LOG_PRINTF_ATTR(fmt_idx, args_idx)
#endif

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_DEBUG = 2
} log_level_t;

/**
 * Configure line-buffered output for stdout and stderr.
 */
void log_init(void);

/**
 * Set the active minimum log level (thread-safe).
 */
void log_set_level(log_level_t level);

/**
 * Get the active minimum log level (thread-safe).
 */
log_level_t log_get_level(void);

/**
 * Format and write a single log line using a va_list.
 */
KOMODO_LOG_PRINTF_ATTR(2, 0)
void log_vwrite(log_level_t level, const char *fmt, va_list ap);

/**
 * Emit a debug log line to stdout (only when LOG_LEVEL=debug).
 */
KOMODO_LOG_PRINTF_ATTR(1, 2)
void log_debug(const char *fmt, ...);

/**
 * Emit an informational log line to stdout.
 */
KOMODO_LOG_PRINTF_ATTR(1, 2)
void log_info(const char *fmt, ...);

/**
 * Emit an error log line to stderr.
 */
KOMODO_LOG_PRINTF_ATTR(1, 2)
void log_error(const char *fmt, ...);

/**
 * Sanitize and truncate untrusted bytes into a single-line printable preview.
 * Escapes control characters, backslashes, and quotes to prevent log injection.
 */
void log_format_preview(const char *src, size_t src_len, char *dst,
                        size_t dst_size);

#endif /* KOMODO_ALERTER_LOGGING_H */
