#ifndef KOMODO_ALERTER_TELEGRAM_H
#define KOMODO_ALERTER_TELEGRAM_H

#include "config.h"

#include <curl/curl.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    CURL *curl;
    struct curl_slist *headers;
    const app_config_t *cfg;
} telegram_client_t;

typedef enum {
    TELEGRAM_OK = 0,
    TELEGRAM_ERR_NETWORK = 1,
    TELEGRAM_ERR_API = 2
} telegram_status_t;

/**
 * Initialize a reusable libcurl-backed Telegram API client.
 * Each worker thread owns one telegram_client_t to reuse TLS/TCP connections
 * safely.
 */
bool telegram_client_init(telegram_client_t *client, const app_config_t *cfg);

/**
 * Clean up libcurl handle and headers owned by the client.
 */
void telegram_client_destroy(telegram_client_t *client);

/**
 * Send a Markdown message via Telegram Bot API sendMessage.
 * On failure, writes a safe error message (never containing the bot token) to
 * err_buf.
 */
telegram_status_t telegram_send_message(telegram_client_t *client,
                                        const char *token, const char *chat_id,
                                        const char *message, const char *sender,
                                        char *err_buf, size_t err_buf_len);

#endif /* KOMODO_ALERTER_TELEGRAM_H */
