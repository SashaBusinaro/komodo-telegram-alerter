#include "config.h"
#include "logging.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool equals_ascii_icase(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool parse_long_env(const char *name, long default_val, long min_val,
                           long max_val, long *out) {
    const char *raw = getenv(name);
    if (raw == NULL || raw[0] == '\0') {
        *out = default_val;
        return true;
    }

    char *endptr = NULL;
    errno = 0;
    long val = strtol(raw, &endptr, 10);
    if (errno != 0 || endptr == raw || *endptr != '\0' || val < min_val ||
        val > max_val) {
        log_error("Invalid %s: '%s' (expected %ld..%ld)", name, raw, min_val,
                  max_val);
        return false;
    }

    *out = val;
    return true;
}

static bool copy_string_checked(char *dst, size_t dst_size, const char *src,
                                const char *name) {
    size_t len = strlen(src);
    if (len >= dst_size) {
        log_error("%s exceeds maximum length (%zu)", name, dst_size - 1);
        return false;
    }
    memcpy(dst, src, len + 1);
    return true;
}

bool config_load_from_env(app_config_t *cfg) {
    if (cfg == NULL) {
        return false;
    }
    memset(cfg, 0, sizeof(*cfg));

    long port_val = 0;
    if (!parse_long_env("PORT", 3000, 1, 65535, &port_val)) {
        return false;
    }
    cfg->port = (uint16_t)port_val;

    const char *bind_addr = getenv("BIND_ADDR");
    if (bind_addr == NULL || bind_addr[0] == '\0') {
        bind_addr = "0.0.0.0";
    }
    if (!copy_string_checked(cfg->bind_addr, sizeof(cfg->bind_addr), bind_addr,
                             "BIND_ADDR")) {
        return false;
    }

    const char *api_url = getenv("TELEGRAM_API_URL");
    if (api_url == NULL || api_url[0] == '\0') {
        api_url = "https://api.telegram.org";
    }
    if (!copy_string_checked(cfg->telegram_api_url,
                             sizeof(cfg->telegram_api_url), api_url,
                             "TELEGRAM_API_URL")) {
        return false;
    }

    size_t api_len = strlen(cfg->telegram_api_url);
    while (api_len > 0 && cfg->telegram_api_url[api_len - 1] == '/') {
        cfg->telegram_api_url[--api_len] = '\0';
    }

    if (strncmp(cfg->telegram_api_url, "https://", 8) == 0 && api_len > 8) {
        cfg->allow_insecure_http = false;
    } else if (strncmp(cfg->telegram_api_url, "http://", 7) == 0 &&
               api_len > 7) {
        cfg->allow_insecure_http = true;
    } else {
        log_error(
            "Invalid TELEGRAM_API_URL: must start with https:// or http://");
        return false;
    }

    /* Match original Node.js proxy precedence: HTTPS_PROXY -> https_proxy ->
     * HTTP_PROXY -> http_proxy */
    static const char *const proxy_vars[] = {"HTTPS_PROXY", "https_proxy",
                                             "HTTP_PROXY", "http_proxy"};
    for (size_t i = 0; i < sizeof(proxy_vars) / sizeof(proxy_vars[0]); i++) {
        const char *val = getenv(proxy_vars[i]);
        if (val != NULL && val[0] != '\0') {
            if (!copy_string_checked(cfg->proxy_url, sizeof(cfg->proxy_url),
                                     val, proxy_vars[i])) {
                return false;
            }
            break;
        }
    }

    long max_body = 0;
    if (!parse_long_env("MAX_BODY_BYTES", 262144, 1024, 10485760, &max_body)) {
        return false;
    }
    cfg->max_body_bytes = (size_t)max_body;

    long workers = 0;
    if (!parse_long_env("WORKER_THREADS", 4, 1, 32, &workers)) {
        return false;
    }
    cfg->worker_threads = (size_t)workers;

    if (!parse_long_env("CONNECT_TIMEOUT_MS", 5000, 100, 60000,
                        &cfg->connect_timeout_ms)) {
        return false;
    }
    if (!parse_long_env("REQUEST_TIMEOUT_MS", 15000, 500, 120000,
                        &cfg->request_timeout_ms)) {
        return false;
    }

    const char *raw_level = getenv("LOG_LEVEL");
    if (raw_level == NULL || raw_level[0] == '\0' ||
        equals_ascii_icase(raw_level, "info")) {
        cfg->log_level = LOG_LEVEL_INFO;
    } else if (equals_ascii_icase(raw_level, "error")) {
        cfg->log_level = LOG_LEVEL_ERROR;
    } else if (equals_ascii_icase(raw_level, "debug")) {
        cfg->log_level = LOG_LEVEL_DEBUG;
    } else {
        log_error("Invalid LOG_LEVEL: '%s' (expected error, info, or debug)",
                  raw_level);
        return false;
    }

    return true;
}
