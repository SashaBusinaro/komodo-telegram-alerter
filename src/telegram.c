#include "telegram.h"
#include "logging.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TELEGRAM_MAX_RESPONSE_BYTES 65536

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
} resp_buf_t;

static size_t curl_write_cb(const char *ptr, size_t size, size_t nmemb,
                            void *userdata) {
    resp_buf_t *buf = (resp_buf_t *)userdata;
    if (size != 0 && nmemb > SIZE_MAX / size) {
        buf->overflow = true;
        return 0;
    }
    size_t chunk = size * nmemb;
    if (chunk == 0) {
        return 0;
    }
    if (buf->len + chunk > TELEGRAM_MAX_RESPONSE_BYTES) {
        buf->overflow = true;
        return 0;
    }
    if (buf->len + chunk + 1 > buf->cap) {
        size_t new_cap = buf->cap == 0 ? 1024 : buf->cap;
        while (new_cap < buf->len + chunk + 1) {
            new_cap *= 2;
        }
        if (new_cap > TELEGRAM_MAX_RESPONSE_BYTES + 1) {
            new_cap = TELEGRAM_MAX_RESPONSE_BYTES + 1;
        }
        char *next = realloc(buf->data, new_cap);
        if (next == NULL) {
            return 0;
        }
        buf->data = next;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, chunk);
    buf->len += chunk;
    buf->data[buf->len] = '\0';
    return chunk;
}

static void set_err(char *err_buf, size_t err_buf_len, const char *msg) {
    if (err_buf != NULL && err_buf_len > 0) {
        snprintf(err_buf, err_buf_len, "%s", msg);
    }
}

bool telegram_client_init(telegram_client_t *client, const app_config_t *cfg) {
    if (client == NULL || cfg == NULL) {
        return false;
    }
    memset(client, 0, sizeof(*client));
    client->cfg = cfg;
    client->curl = curl_easy_init();
    if (client->curl == NULL) {
        return false;
    }
    client->headers = curl_slist_append(NULL, "Content-Type: application/json");
    if (client->headers == NULL) {
        curl_easy_cleanup(client->curl);
        client->curl = NULL;
        return false;
    }
    return true;
}

void telegram_client_destroy(telegram_client_t *client) {
    if (client == NULL) {
        return;
    }
    if (client->headers != NULL) {
        curl_slist_free_all(client->headers);
        client->headers = NULL;
    }
    if (client->curl != NULL) {
        curl_easy_cleanup(client->curl);
        client->curl = NULL;
    }
}

telegram_status_t telegram_send_message(telegram_client_t *client,
                                        const char *token, const char *chat_id,
                                        const char *message, const char *sender,
                                        char *err_buf, size_t err_buf_len) {
    const char *sender_sep = (sender != NULL && sender[0] != '\0') ? ", " : "";
    const char *sender_str =
        (sender != NULL && sender[0] != '\0') ? sender : "";
    if (client == NULL || client->curl == NULL || client->cfg == NULL ||
        token == NULL || chat_id == NULL || message == NULL) {
        set_err(err_buf, err_buf_len, "Failed to send message to Telegram");
        return TELEGRAM_ERR_NETWORK;
    }

    /* Build JSON request payload: {"chat_id": ..., "text": ..., "parse_mode":
     * "Markdown"} */
    struct json_object *payload = json_object_new_object();
    if (payload == NULL) {
        set_err(err_buf, err_buf_len, "Failed to send message to Telegram");
        return TELEGRAM_ERR_NETWORK;
    }

    struct json_object *j_chat = json_object_new_string(chat_id);
    struct json_object *j_text = json_object_new_string(message);
    struct json_object *j_mode = json_object_new_string("Markdown");
    if (j_chat == NULL || j_text == NULL || j_mode == NULL ||
        json_object_object_add(payload, "chat_id", j_chat) != 0 ||
        json_object_object_add(payload, "text", j_text) != 0 ||
        json_object_object_add(payload, "parse_mode", j_mode) != 0) {
        if (j_chat != NULL)
            json_object_put(j_chat);
        if (j_text != NULL)
            json_object_put(j_text);
        if (j_mode != NULL)
            json_object_put(j_mode);
        json_object_put(payload);
        set_err(err_buf, err_buf_len, "Failed to send message to Telegram");
        return TELEGRAM_ERR_NETWORK;
    }

    size_t body_len = 0;
    const char *body_str = json_object_to_json_string_length(
        payload, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE,
        &body_len);
    if (body_str == NULL) {
        json_object_put(payload);
        set_err(err_buf, err_buf_len, "Failed to send message to Telegram");
        return TELEGRAM_ERR_NETWORK;
    }

    /* Construct URL: <base>/bot<token>/sendMessage (never logged) */
    char url[768];
    int url_len = snprintf(url, sizeof(url), "%s/bot%s/sendMessage",
                           client->cfg->telegram_api_url, token);
    if (url_len < 0 || (size_t)url_len >= sizeof(url)) {
        json_object_put(payload);
        set_err(err_buf, err_buf_len, "Failed to send message to Telegram");
        return TELEGRAM_ERR_NETWORK;
    }

    resp_buf_t resp = {0};
    CURL *curl = client->curl;

    /* curl_easy_reset resets options while retaining connection/TLS session
     * cache */
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, client->headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     client->cfg->connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, client->cfg->request_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR,
                     client->cfg->allow_insecure_http ? "http,https" : "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                     client->cfg->allow_insecure_http
                         ? (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS)
                         : (long)CURLPROTO_HTTPS);
#endif

    if (client->cfg->proxy_url[0] != '\0') {
        curl_easy_setopt(curl, CURLOPT_PROXY, client->cfg->proxy_url);
    }

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    /* Clear URL containing token from stack and free request JSON payload */
    memset(url, 0, sizeof(url));
    json_object_put(payload);

    if (rc != CURLE_OK || resp.overflow || resp.data == NULL) {
        log_error(
            "Failed to send message to Telegram: %s (curl=%d, http=%ld%s%s)",
            resp.overflow ? "response too large" : curl_easy_strerror(rc),
            (int)rc, http_code, sender_sep, sender_str);
        free(resp.data);
        set_err(err_buf, err_buf_len, "Failed to send message to Telegram");
        return TELEGRAM_ERR_NETWORK;
    }

    struct json_tokener *tok = json_tokener_new();
    if (tok == NULL) {
        free(resp.data);
        set_err(err_buf, err_buf_len, "Failed to send message to Telegram");
        return TELEGRAM_ERR_NETWORK;
    }
    struct json_object *resp_json =
        json_tokener_parse_ex(tok, resp.data, (int)resp.len);
    enum json_tokener_error jerr = json_tokener_get_error(tok);
    json_tokener_free(tok);

    if (jerr != json_tokener_success || resp_json == NULL ||
        !json_object_is_type(resp_json, json_type_object)) {
        if (resp_json != NULL) {
            json_object_put(resp_json);
        }
        char preview[201];
        log_format_preview(resp.data, resp.len, preview, sizeof(preview));
        free(resp.data);
        log_error("Failed to send message to Telegram: invalid JSON response "
                  "(http=%ld, %zu bytes, %s, preview=\"%s\"%s%s)",
                  http_code, resp.len, json_tokener_error_desc(jerr), preview,
                  sender_sep, sender_str);
        set_err(err_buf, err_buf_len, "Failed to send message to Telegram");
        return TELEGRAM_ERR_NETWORK;
    }
    free(resp.data);

    struct json_object *ok_obj = NULL;
    if (json_object_object_get_ex(resp_json, "ok", &ok_obj) && ok_obj != NULL &&
        json_object_is_type(ok_obj, json_type_boolean) &&
        json_object_get_boolean(ok_obj)) {
        json_object_put(resp_json);
        log_debug("Telegram API sendMessage succeeded (http=%ld%s%s)",
                  http_code, sender_sep, sender_str);
        return TELEGRAM_OK;
    }

    const char *desc = "Unknown Telegram API error";
    struct json_object *desc_obj = NULL;
    if (json_object_object_get_ex(resp_json, "description", &desc_obj) &&
        desc_obj != NULL && json_object_is_type(desc_obj, json_type_string)) {
        const char *s = json_object_get_string(desc_obj);
        if (s != NULL && s[0] != '\0') {
            desc = s;
        }
    }

    log_error("Telegram API error (http=%ld%s%s): %s", http_code, sender_sep,
              sender_str, desc);
    set_err(err_buf, err_buf_len, desc);
    json_object_put(resp_json);
    return TELEGRAM_ERR_API;
}
