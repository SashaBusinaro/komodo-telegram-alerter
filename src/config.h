#ifndef KOMODO_ALERTER_CONFIG_H
#define KOMODO_ALERTER_CONFIG_H

#include "logging.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t port;
    char bind_addr[64];
    char telegram_api_url[512];
    bool allow_insecure_http;
    char proxy_url[512];
    size_t max_body_bytes;
    size_t worker_threads;
    long connect_timeout_ms;
    long request_timeout_ms;
    log_level_t log_level;
} app_config_t;

/**
 * Load runtime configuration from environment variables with validation.
 * Returns true on success, false on invalid configuration (error logged to
 * stderr).
 */
bool config_load_from_env(app_config_t *cfg);

#endif /* KOMODO_ALERTER_CONFIG_H */
