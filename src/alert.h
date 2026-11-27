#ifndef KOMODO_ALERTER_ALERT_H
#define KOMODO_ALERTER_ALERT_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Validate Telegram bot token format: ^[0-9]+:[A-Za-z0-9_-]+$ (1..128 bytes).
 * Prevents URL/path/SSRF injection when constructing Telegram API URLs.
 */
bool validate_telegram_token(const char *token);

/**
 * Validate Telegram chat_id format: ^-?[0-9]+$ or ^@[A-Za-z0-9_]{5,}$ (1..64
 * bytes).
 */
bool validate_telegram_chat_id(const char *chat_id);

/**
 * Parse a Komodo alert JSON object and format the Telegram Markdown message
 * with exact parity to the legacy Node.js implementation.
 *
 * Returns a heap-allocated NUL-terminated string owned by the caller, or NULL
 * on invalid JSON / non-object payload / allocation failure (writing a message
 * into err_buf when provided).
 */
char *format_komodo_alert(const char *json_body, size_t body_len, char *err_buf,
                          size_t err_buf_len);

#endif /* KOMODO_ALERTER_ALERT_H */
