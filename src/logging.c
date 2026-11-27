#include "logging.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

static atomic_int g_log_level = LOG_LEVEL_INFO;

void log_init(void) {
    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    (void)setvbuf(stderr, NULL, _IOLBF, 0);
}

void log_set_level(log_level_t level) {
    atomic_store_explicit(&g_log_level, (int)level, memory_order_release);
}

log_level_t log_get_level(void) {
    int raw = atomic_load_explicit(&g_log_level, memory_order_acquire);
    if (raw <= (int)LOG_LEVEL_ERROR) {
        return LOG_LEVEL_ERROR;
    }
    if (raw >= (int)LOG_LEVEL_DEBUG) {
        return LOG_LEVEL_DEBUG;
    }
    return LOG_LEVEL_INFO;
}

void log_vwrite(log_level_t level, const char *fmt, va_list ap) {
    if ((int)level > (int)log_get_level()) {
        return;
    }

    FILE *stream = (level == LOG_LEVEL_ERROR) ? stderr : stdout;
    const char *tag = "INFO";
    if (level == LOG_LEVEL_ERROR) {
        tag = "ERROR";
    } else if (level == LOG_LEVEL_DEBUG) {
        tag = "DEBUG";
    }

    char msg[2048];
    int written = vsnprintf(msg, sizeof(msg), fmt, ap);
    if (written < 0) {
        (void)fprintf(stream, "[%s] (log formatting error)\n", tag);
        return;
    }
    (void)fprintf(stream, "[%s] %s\n", tag, msg);
}

void log_debug(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vwrite(LOG_LEVEL_DEBUG, fmt, ap);
    va_end(ap);
}

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vwrite(LOG_LEVEL_INFO, fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vwrite(LOG_LEVEL_ERROR, fmt, ap);
    va_end(ap);
}

static size_t escape_preview_byte(unsigned char c, char out[2]) {
    switch (c) {
    case '\n':
        out[0] = '\\';
        out[1] = 'n';
        return 2;
    case '\r':
        out[0] = '\\';
        out[1] = 'r';
        return 2;
    case '\t':
        out[0] = '\\';
        out[1] = 't';
        return 2;
    case '"':
        out[0] = '\\';
        out[1] = '"';
        return 2;
    case '\\':
        out[0] = '\\';
        out[1] = '\\';
        return 2;
    default:
        if (c < 0x20 || c == 0x7F) {
            out[0] = '?';
            return 1;
        }
        out[0] = (char)c;
        return 1;
    }
}

void log_format_preview(const char *src, size_t src_len, char *dst,
                        size_t dst_size) {
    if (dst == NULL || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL || src_len == 0) {
        return;
    }

    size_t max_chars = dst_size - 1;
    size_t out_len = 0;
    bool truncated = false;

    for (size_t i = 0; i < src_len; i++) {
        char seq[2];
        size_t seq_len = escape_preview_byte((unsigned char)src[i], seq);
        if (out_len + seq_len > max_chars) {
            truncated = true;
            break;
        }
        for (size_t j = 0; j < seq_len; j++) {
            dst[out_len++] = seq[j];
        }
    }

    if (truncated) {
        if (max_chars >= 3) {
            size_t cut = out_len;
            if (cut > max_chars - 3) {
                cut = max_chars - 3;
            }
            /* Avoid leaving a dangling backslash before "..." */
            if (cut > 0 && dst[cut - 1] == '\\') {
                size_t bs = 0;
                size_t k = cut;
                while (k > 0 && dst[k - 1] == '\\') {
                    bs++;
                    k--;
                }
                if ((bs % 2) != 0) {
                    cut--;
                }
            }
            dst[cut++] = '.';
            dst[cut++] = '.';
            dst[cut++] = '.';
            out_len = cut;
        }
    }

    dst[out_len] = '\0';
}
