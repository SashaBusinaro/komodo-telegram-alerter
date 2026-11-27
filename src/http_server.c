#include "http_server.h"
#include "alert.h"
#include "logging.h"
#include "telegram.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <llhttp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_URL_BYTES 2048
#define MAX_HEADER_BYTES 8192
#define MAX_PEER_ADDR_BYTES 64
#define QUEUE_CAPACITY 64
#define SOCKET_TIMEOUT_SEC 5
#define READ_DEADLINE_SEC 10

typedef struct {
    int fd;
    char peer[MAX_PEER_ADDR_BYTES];
} client_conn_t;

typedef struct {
    client_conn_t items[QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
} fd_queue_t;

struct http_server {
    const app_config_t *cfg;
    int listen_fd;
    uint16_t bound_port;
    int wake_pipe[2];
    atomic_bool stopping;
    bool mu_initialized;
    bool cond_initialized;

    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    fd_queue_t queue;

    pthread_t *threads;
    size_t num_threads;
    size_t busy_workers;
    size_t available_workers;
};

static int hex_val(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool url_decode(const char *src, size_t src_len, char *out,
                       size_t out_size) {
    if (out == NULL || out_size == 0) {
        return false;
    }
    size_t out_idx = 0;
    for (size_t i = 0; i < src_len; i++) {
        char ch = src[i];
        if (ch == '%') {
            if (i + 2 >= src_len) {
                return false;
            }
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            unsigned char decoded = (unsigned char)((hi << 4) | lo);
            if (decoded == '\0') {
                return false;
            }
            if (out_idx + 1 >= out_size) {
                return false;
            }
            out[out_idx++] = (char)decoded;
            i += 2;
        } else if (ch == '+') {
            if (out_idx + 1 >= out_size) {
                return false;
            }
            out[out_idx++] = ' ';
        } else {
            if (out_idx + 1 >= out_size) {
                return false;
            }
            out[out_idx++] = ch;
        }
    }
    out[out_idx] = '\0';
    return true;
}

bool http_query_get_param(const char *query, size_t query_len, const char *key,
                          char *out, size_t out_size, bool *found) {
    if (found != NULL) {
        *found = false;
    }
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (key == NULL || key[0] == '\0' || out == NULL || out_size == 0) {
        return false;
    }
    if (query == NULL || query_len == 0) {
        return true;
    }

    size_t key_len = strlen(key);
    size_t pos = 0;
    while (pos < query_len) {
        const char *amp = memchr(query + pos, '&', query_len - pos);
        size_t seg_len =
            amp != NULL ? (size_t)(amp - (query + pos)) : (query_len - pos);
        const char *seg = query + pos;

        const char *eq = memchr(seg, '=', seg_len);
        size_t seg_key_len = eq != NULL ? (size_t)(eq - seg) : seg_len;
        if (seg_key_len == key_len && memcmp(seg, key, key_len) == 0) {
            if (eq == NULL) {
                out[0] = '\0';
                if (found != NULL) {
                    *found = true;
                }
            } else {
                const char *val = eq + 1;
                size_t val_len = seg_len - seg_key_len - 1;
                if (!url_decode(val, val_len, out, out_size)) {
                    return false;
                }
                if (found != NULL) {
                    *found = true;
                }
            }
        }

        if (amp == NULL) {
            break;
        }
        pos += seg_len + 1;
    }

    return true;
}

static bool write_all(int fd, const char *buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = send(fd, buf + written, len - written, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        written += (size_t)n;
    }
    return true;
}

static const char *status_reason(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Payload Too Large";
    case 414:
        return "URI Too Long";
    case 429:
        return "Too Many Requests";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Error";
    }
}

static void send_json_raw(int fd, int status, const char *json_body) {
    size_t body_len = strlen(json_body);
    const char *retry_after =
        (status == 429 || status == 503) ? "Retry-After: 1\r\n" : "";
    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "X-Content-Type-Options: nosniff\r\n"
                        "Cache-Control: no-store\r\n"
                        "%s\r\n",
                        status, status_reason(status), body_len, retry_after);
    if (hlen <= 0 || (size_t)hlen >= sizeof(header)) {
        return;
    }
    if (write_all(fd, header, (size_t)hlen)) {
        (void)write_all(fd, json_body, body_len);
    }
}

static void send_json_error(int fd, int status, const char *err_msg) {
    struct json_object *obj = json_object_new_object();
    struct json_object *jerr =
        json_object_new_string(err_msg != NULL ? err_msg : "Unknown error");
    if (obj == NULL || jerr == NULL ||
        json_object_object_add(obj, "error", jerr) != 0) {
        if (jerr != NULL && obj == NULL)
            json_object_put(jerr);
        if (obj != NULL)
            json_object_put(obj);
        send_json_raw(fd, status, "{\"error\":\"Internal error\"}");
        return;
    }

    const char *serialized = json_object_to_json_string_ext(
        obj, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
    send_json_raw(fd, status,
                  serialized != NULL ? serialized
                                     : "{\"error\":\"Internal error\"}");
    json_object_put(obj);
}

void http_format_peer_addr(const struct sockaddr *addr, socklen_t addr_len,
                           char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
    (void)snprintf(out, out_size, "%s", "unknown");
    if (addr == NULL) {
        return;
    }

    char formatted[MAX_PEER_ADDR_BYTES];
    int n = -1;

    if (addr->sa_family == AF_INET) {
        if (addr_len < (socklen_t)sizeof(struct sockaddr_in)) {
            return;
        }
        struct sockaddr_in sin;
        memcpy(&sin, addr, sizeof(sin));
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sin.sin_addr, ip, sizeof(ip)) == NULL) {
            return;
        }
        n = snprintf(formatted, sizeof(formatted), "%s:%u", ip,
                     (unsigned)ntohs(sin.sin_port));
    } else if (addr->sa_family == AF_INET6) {
        if (addr_len < (socklen_t)sizeof(struct sockaddr_in6)) {
            return;
        }
        struct sockaddr_in6 sin6;
        memcpy(&sin6, addr, sizeof(sin6));
        static const unsigned char v4_mapped_prefix[12] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        unsigned port = (unsigned)ntohs(sin6.sin6_port);
        if (memcmp(sin6.sin6_addr.s6_addr, v4_mapped_prefix,
                   sizeof(v4_mapped_prefix)) == 0) {
            struct in_addr v4;
            memcpy(&v4, &sin6.sin6_addr.s6_addr[12], sizeof(v4));
            char ip[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &v4, ip, sizeof(ip)) == NULL) {
                return;
            }
            n = snprintf(formatted, sizeof(formatted), "%s:%u", ip, port);
        } else {
            char ip[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, &sin6.sin6_addr, ip, sizeof(ip)) == NULL) {
                return;
            }
            n = snprintf(formatted, sizeof(formatted), "[%s]:%u", ip, port);
        }
    }

    if (n > 0 && (size_t)n < out_size) {
        memcpy(out, formatted, (size_t)n + 1);
    }
}

typedef enum {
    HDR_CAPTURE_NONE = 0,
    HDR_CAPTURE_CONTENT_TYPE,
    HDR_CAPTURE_X_FORWARDED_FOR,
    HDR_CAPTURE_X_REAL_IP,
    HDR_CAPTURE_USER_AGENT
} header_capture_t;

typedef struct {
    const app_config_t *cfg;
    size_t url_len;
    size_t header_bytes;
    size_t content_type_len;
    size_t x_forwarded_for_len;
    size_t x_real_ip_len;
    size_t user_agent_len;
    size_t header_name_len;
    char *body;
    size_t body_len;
    size_t body_cap;
    header_capture_t header_capture;
    int error_status;
    bool header_name_overflow;
    bool header_field_started;
    bool complete;
    char header_name[32];
    char peer[MAX_PEER_ADDR_BYTES];
    char content_type[65];
    char x_real_ip[65];
    char x_forwarded_for[129];
    char user_agent[129];
    char url[MAX_URL_BYTES + 1];
} req_ctx_t;

static void append_header_value(char *dst, size_t dst_size, size_t *len,
                                const char *at, size_t n) {
    if (dst == NULL || dst_size == 0 || len == NULL || *len >= dst_size - 1) {
        return;
    }
    size_t avail = dst_size - 1 - *len;
    size_t to_copy = n < avail ? n : avail;
    if (to_copy > 0) {
        memcpy(dst + *len, at, to_copy);
        *len += to_copy;
        dst[*len] = '\0';
    }
}

static void trim_trailing_ows(char *buf, size_t *len) {
    while (*len > 0 && (buf[*len - 1] == ' ' || buf[*len - 1] == '\t')) {
        buf[--(*len)] = '\0';
    }
}

static int on_url_cb(llhttp_t *parser, const char *at, size_t length) {
    req_ctx_t *ctx = (req_ctx_t *)parser->data;
    if (length > MAX_URL_BYTES - ctx->url_len) {
        ctx->error_status = 414;
        return HPE_USER;
    }
    memcpy(ctx->url + ctx->url_len, at, length);
    ctx->url_len += length;
    ctx->url[ctx->url_len] = '\0';
    return 0;
}

static int on_header_field_cb(llhttp_t *parser, const char *at, size_t length) {
    req_ctx_t *ctx = (req_ctx_t *)parser->data;
    if (length > MAX_HEADER_BYTES - ctx->header_bytes) {
        ctx->error_status = 431;
        return HPE_USER;
    }
    ctx->header_bytes += length;

    if (!ctx->header_field_started) {
        ctx->header_name_len = 0;
        ctx->header_name_overflow = false;
        ctx->header_capture = HDR_CAPTURE_NONE;
        ctx->header_field_started = true;
    }
    for (size_t i = 0; i < length; i++) {
        if (ctx->header_name_len + 1 < sizeof(ctx->header_name)) {
            ctx->header_name[ctx->header_name_len++] =
                (char)tolower((unsigned char)at[i]);
        } else {
            ctx->header_name_overflow = true;
            break;
        }
    }
    return 0;
}

static int on_header_field_complete_cb(llhttp_t *parser) {
    req_ctx_t *ctx = (req_ctx_t *)parser->data;
    ctx->header_field_started = false;
    ctx->header_capture = HDR_CAPTURE_NONE;
    if (ctx->header_name_overflow) {
        return 0;
    }

    if (ctx->header_name_len == 12 &&
        memcmp(ctx->header_name, "content-type", 12) == 0) {
        ctx->header_capture = HDR_CAPTURE_CONTENT_TYPE;
        ctx->content_type_len = 0;
        ctx->content_type[0] = '\0';
    } else if (ctx->header_name_len == 15 &&
               memcmp(ctx->header_name, "x-forwarded-for", 15) == 0) {
        ctx->header_capture = HDR_CAPTURE_X_FORWARDED_FOR;
        if (ctx->x_forwarded_for_len > 0) {
            append_header_value(ctx->x_forwarded_for,
                                sizeof(ctx->x_forwarded_for),
                                &ctx->x_forwarded_for_len, ", ", 2);
        }
    } else if (ctx->header_name_len == 9 &&
               memcmp(ctx->header_name, "x-real-ip", 9) == 0) {
        ctx->header_capture = HDR_CAPTURE_X_REAL_IP;
        ctx->x_real_ip_len = 0;
        ctx->x_real_ip[0] = '\0';
    } else if (ctx->header_name_len == 10 &&
               memcmp(ctx->header_name, "user-agent", 10) == 0) {
        ctx->header_capture = HDR_CAPTURE_USER_AGENT;
        ctx->user_agent_len = 0;
        ctx->user_agent[0] = '\0';
    }
    return 0;
}

static int on_header_value_cb(llhttp_t *parser, const char *at, size_t length) {
    req_ctx_t *ctx = (req_ctx_t *)parser->data;
    if (length > MAX_HEADER_BYTES - ctx->header_bytes) {
        ctx->error_status = 431;
        return HPE_USER;
    }
    ctx->header_bytes += length;

    switch (ctx->header_capture) {
    case HDR_CAPTURE_CONTENT_TYPE:
        append_header_value(ctx->content_type, sizeof(ctx->content_type),
                            &ctx->content_type_len, at, length);
        break;
    case HDR_CAPTURE_X_FORWARDED_FOR:
        append_header_value(ctx->x_forwarded_for, sizeof(ctx->x_forwarded_for),
                            &ctx->x_forwarded_for_len, at, length);
        break;
    case HDR_CAPTURE_X_REAL_IP:
        append_header_value(ctx->x_real_ip, sizeof(ctx->x_real_ip),
                            &ctx->x_real_ip_len, at, length);
        break;
    case HDR_CAPTURE_USER_AGENT:
        append_header_value(ctx->user_agent, sizeof(ctx->user_agent),
                            &ctx->user_agent_len, at, length);
        break;
    case HDR_CAPTURE_NONE:
        break;
    }
    return 0;
}

static int on_header_value_complete_cb(llhttp_t *parser) {
    req_ctx_t *ctx = (req_ctx_t *)parser->data;
    switch (ctx->header_capture) {
    case HDR_CAPTURE_CONTENT_TYPE:
        trim_trailing_ows(ctx->content_type, &ctx->content_type_len);
        break;
    case HDR_CAPTURE_X_FORWARDED_FOR:
        trim_trailing_ows(ctx->x_forwarded_for, &ctx->x_forwarded_for_len);
        break;
    case HDR_CAPTURE_X_REAL_IP:
        trim_trailing_ows(ctx->x_real_ip, &ctx->x_real_ip_len);
        break;
    case HDR_CAPTURE_USER_AGENT:
        trim_trailing_ows(ctx->user_agent, &ctx->user_agent_len);
        break;
    case HDR_CAPTURE_NONE:
        break;
    }
    ctx->header_capture = HDR_CAPTURE_NONE;
    return 0;
}

static int on_headers_complete_cb(llhttp_t *parser) {
    req_ctx_t *ctx = (req_ctx_t *)parser->data;
    if ((parser->flags & F_CONTENT_LENGTH) &&
        parser->content_length > ctx->cfg->max_body_bytes) {
        ctx->error_status = 413;
        return -1;
    }
    if ((parser->flags & F_CONTENT_LENGTH) && parser->content_length > 0) {
        size_t cl = (size_t)parser->content_length;
        ctx->body = malloc(cl + 1);
        if (ctx->body == NULL) {
            ctx->error_status = 500;
            return -1;
        }
        ctx->body_cap = cl + 1;
        ctx->body[0] = '\0';
    }
    return 0;
}

static int on_body_cb(llhttp_t *parser, const char *at, size_t length) {
    req_ctx_t *ctx = (req_ctx_t *)parser->data;
    if (length > ctx->cfg->max_body_bytes - ctx->body_len) {
        ctx->error_status = 413;
        return HPE_USER;
    }
    if (ctx->body_len + length + 1 > ctx->body_cap) {
        size_t new_cap = ctx->body_cap == 0 ? 1024 : ctx->body_cap;
        while (new_cap < ctx->body_len + length + 1) {
            if (new_cap > SIZE_MAX / 2) {
                new_cap = ctx->body_len + length + 1;
                break;
            }
            new_cap *= 2;
        }
        if (new_cap > ctx->cfg->max_body_bytes + 1) {
            new_cap = ctx->cfg->max_body_bytes + 1;
        }
        char *next = realloc(ctx->body, new_cap);
        if (next == NULL) {
            ctx->error_status = 500;
            return HPE_USER;
        }
        ctx->body = next;
        ctx->body_cap = new_cap;
    }
    memcpy(ctx->body + ctx->body_len, at, length);
    ctx->body_len += length;
    ctx->body[ctx->body_len] = '\0';
    return 0;
}

static int on_message_complete_cb(llhttp_t *parser) {
    req_ctx_t *ctx = (req_ctx_t *)parser->data;
    ctx->complete = true;
    return 0;
}

static void format_sender_info(const req_ctx_t *ctx, char *out,
                               size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
    const char *peer =
        (ctx != NULL && ctx->peer[0] != '\0') ? ctx->peer : "unknown";
    int n = snprintf(out, out_size, "from=%s", peer);
    if (n < 0 || (size_t)n >= out_size) {
        out[out_size - 1] = '\0';
        return;
    }
    if (ctx == NULL) {
        return;
    }
    size_t off = (size_t)n;

    char xff_preview[97] = {0};
    if (ctx->x_forwarded_for_len > 0) {
        log_format_preview(ctx->x_forwarded_for, ctx->x_forwarded_for_len,
                           xff_preview, sizeof(xff_preview));
        int w =
            snprintf(out + off, out_size - off, ", xff=\"%s\"", xff_preview);
        if (w < 0 || (size_t)w >= out_size - off) {
            return;
        }
        off += (size_t)w;
    }

    if (ctx->x_real_ip_len > 0) {
        char rip_preview[65];
        log_format_preview(ctx->x_real_ip, ctx->x_real_ip_len, rip_preview,
                           sizeof(rip_preview));
        if (ctx->x_forwarded_for_len == 0 ||
            strcmp(rip_preview, xff_preview) != 0) {
            int w = snprintf(out + off, out_size - off, ", real_ip=\"%s\"",
                             rip_preview);
            if (w < 0 || (size_t)w >= out_size - off) {
                return;
            }
            off += (size_t)w;
        }
    }

    if (ctx->user_agent_len > 0) {
        char ua_preview[97];
        log_format_preview(ctx->user_agent, ctx->user_agent_len, ua_preview,
                           sizeof(ua_preview));
        (void)snprintf(out + off, out_size - off, ", ua=\"%s\"", ua_preview);
    }
}

static void handle_alert_request(int fd, const req_ctx_t *ctx,
                                 const char *query, size_t query_len,
                                 const char *sender, telegram_client_t *tg) {
    char token[256];
    char chat_id[128];
    bool has_token = false;
    bool has_chat_id = false;

    if (!http_query_get_param(query, query_len, "token", token, sizeof(token),
                              &has_token) ||
        !http_query_get_param(query, query_len, "chat_id", chat_id,
                              sizeof(chat_id), &has_chat_id)) {
        memset(token, 0, sizeof(token));
        log_error("Invalid query encoding on POST /alert (query_len=%zu, %s)",
                  query_len, sender);
        send_json_error(fd, 400, "Invalid query encoding");
        return;
    }

    bool missing_token = !has_token || token[0] == '\0';
    bool missing_chat = !has_chat_id || chat_id[0] == '\0';
    if (missing_token || missing_chat) {
        memset(token, 0, sizeof(token));
        log_error("Missing required query parameter(s):%s%s (%s)",
                  missing_token ? " token" : "", missing_chat ? " chat_id" : "",
                  sender);
        send_json_error(fd, 400, "Missing token or chat_id");
        return;
    }

    bool token_ok = validate_telegram_token(token);
    bool chat_ok = validate_telegram_chat_id(chat_id);
    if (!token_ok || !chat_ok) {
        memset(token, 0, sizeof(token));
        log_error("Invalid query parameter format:%s%s (%s)",
                  !token_ok ? " token (expected <digits>:<secret>)" : "",
                  !chat_ok ? " chat_id (expected numeric ID or @username)" : "",
                  sender);
        send_json_error(fd, 400, "Invalid token or chat_id");
        return;
    }

    char err_buf[256];
    char *message =
        format_komodo_alert(ctx->body, ctx->body_len, err_buf, sizeof(err_buf));
    if (message == NULL) {
        memset(token, 0, sizeof(token));
        char preview[201];
        char ct_preview[65];
        log_format_preview(ctx->body, ctx->body_len, preview, sizeof(preview));
        log_format_preview(ctx->content_type, ctx->content_type_len, ct_preview,
                           sizeof(ct_preview));
        log_error(
            "Invalid JSON payload: %s (body_len=%zu, content_type=\"%s\", "
            "preview=\"%s\", %s)",
            err_buf[0] != '\0' ? err_buf : "parse failed", ctx->body_len,
            ct_preview[0] != '\0' ? ct_preview : "(none)", preview, sender);
        send_json_error(fd, 400, "Invalid JSON payload");
        return;
    }

    telegram_status_t st = telegram_send_message(
        tg, token, chat_id, message, sender, err_buf, sizeof(err_buf));
    memset(token, 0, sizeof(token));
    free(message);

    if (st != TELEGRAM_OK) {
        send_json_error(fd, 500, err_buf);
        return;
    }

    log_info("Message sent successfully (%s)", sender);
    send_json_raw(fd, 200, "{\"success\":true}");
}

static bool set_blocking_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        return false;
    }
    int fdflags = fcntl(fd, F_GETFD, 0);
    if (fdflags < 0 || fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) < 0) {
        return false;
    }
    return true;
}

static void configure_client_socket(int fd) {
    (void)set_blocking_cloexec(fd);
    struct timeval tv = {.tv_sec = SOCKET_TIMEOUT_SEC, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static bool read_and_parse_request(int fd, req_ctx_t *ctx,
                                   uint8_t *out_method) {
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_url = on_url_cb;
    settings.on_header_field = on_header_field_cb;
    settings.on_header_field_complete = on_header_field_complete_cb;
    settings.on_header_value = on_header_value_cb;
    settings.on_header_value_complete = on_header_value_complete_cb;
    settings.on_headers_complete = on_headers_complete_cb;
    settings.on_body = on_body_cb;
    settings.on_message_complete = on_message_complete_cb;

    llhttp_t parser;
    llhttp_init(&parser, HTTP_REQUEST, &settings);
    parser.data = ctx;

    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    char read_buf[4096];
    for (;;) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        if (now_ts.tv_sec - start_ts.tv_sec >= READ_DEADLINE_SEC) {
            ctx->error_status = 400;
            char sender[384];
            format_sender_info(ctx, sender, sizeof(sender));
            log_error("HTTP request read deadline exceeded (%ds, body_len=%zu, "
                      "%s)",
                      READ_DEADLINE_SEC, ctx->body_len, sender);
            return false;
        }

        ssize_t n = recv(fd, read_buf, sizeof(read_buf), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            char sender[384];
            format_sender_info(ctx, sender, sizeof(sender));
            log_error("HTTP request read failed: %s (%s)", strerror(errno),
                      sender);
            return false;
        }
        if (n == 0) {
            llhttp_errno_t ferr = llhttp_finish(&parser);
            if (ferr == HPE_OK && ctx->complete) {
                *out_method = llhttp_get_method(&parser);
                return true;
            }
            char sender[384];
            format_sender_info(ctx, sender, sizeof(sender));
            log_error("HTTP request incomplete at EOF: %s (url_len=%zu, "
                      "body_len=%zu, %s)",
                      llhttp_errno_name(ferr), ctx->url_len, ctx->body_len,
                      sender);
            return false;
        }

        llhttp_errno_t perr = llhttp_execute(&parser, read_buf, (size_t)n);
        if (perr != HPE_OK) {
            if (ctx->error_status == 0) {
                ctx->error_status = 400;
            }
            char sender[384];
            format_sender_info(ctx, sender, sizeof(sender));
            if (ctx->error_status == 413) {
                log_error("HTTP request rejected (413): payload exceeds "
                          "MAX_BODY_BYTES=%zu (content_length=%llu, "
                          "body_len=%zu, %s)",
                          ctx->cfg->max_body_bytes,
                          (unsigned long long)parser.content_length,
                          ctx->body_len, sender);
            } else {
                log_error("HTTP request parse error (%d): %s (url_len=%zu, "
                          "header_bytes=%zu, body_len=%zu, %s)",
                          ctx->error_status, llhttp_errno_name(perr),
                          ctx->url_len, ctx->header_bytes, ctx->body_len,
                          sender);
            }
            return false;
        }
        if (ctx->complete) {
            *out_method = llhttp_get_method(&parser);
            return true;
        }
    }
}

static const char *parse_error_message(int status) {
    switch (status) {
    case 413:
        return "Payload too large";
    case 414:
        return "URI too long";
    case 431:
        return "Request header fields too large";
    case 500:
        return "Internal server error";
    default:
        return "Bad request";
    }
}

static void send_parse_error_and_drain(int fd, int error_status) {
    int status = error_status != 0 ? error_status : 400;
    send_json_error(fd, status, parse_error_message(status));

    /* Lingering half-close so clients reading early errors (e.g. 413) don't
     * hit TCP RST */
    (void)shutdown(fd, SHUT_WR);
    char drain_buf[4096];
    size_t drained = 0;
    for (int iter = 0; iter < 5 && drained < 65536; iter++) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, 20) <= 0 || !(pfd.revents & POLLIN)) {
            break;
        }
        ssize_t r = recv(fd, drain_buf, sizeof(drain_buf), 0);
        if (r <= 0) {
            break;
        }
        drained += (size_t)r;
    }
}

static void dispatch_request(int fd, uint8_t method, const req_ctx_t *ctx,
                             telegram_client_t *tg) {
    const char *qmark = memchr(ctx->url, '?', ctx->url_len);
    size_t path_len = qmark != NULL ? (size_t)(qmark - ctx->url) : ctx->url_len;
    const char *query = qmark != NULL ? qmark + 1 : "";
    size_t query_len = qmark != NULL ? ctx->url_len - path_len - 1 : 0;

    char sender[384];
    format_sender_info(ctx, sender, sizeof(sender));

    char path_preview[129];
    log_format_preview(ctx->url, path_len, path_preview, sizeof(path_preview));
    const char *method_str = llhttp_method_name((llhttp_method_t)method);
    log_debug("Request: %s %s (body_len=%zu, %s)", method_str, path_preview,
              ctx->body_len, sender);

    if (path_len == 7 && memcmp(ctx->url, "/health", 7) == 0) {
        if (method == HTTP_GET) {
            send_json_raw(fd, 200, "{\"status\":\"ok\"}");
        } else {
            log_error("Method not allowed (405): %s %s (%s)", method_str,
                      path_preview, sender);
            send_json_error(fd, 405, "Method not allowed");
        }
    } else if (path_len == 6 && memcmp(ctx->url, "/alert", 6) == 0) {
        if (method == HTTP_POST) {
            handle_alert_request(fd, ctx, query, query_len, sender, tg);
        } else {
            log_error("Method not allowed (405): %s %s (%s)", method_str,
                      path_preview, sender);
            send_json_error(fd, 405, "Method not allowed");
        }
    } else {
        log_error("Route not found (404): %s %s (%s)", method_str, path_preview,
                  sender);
        send_json_error(fd, 404, "Not found");
    }
}

static void handle_client(int fd, const char *peer, const app_config_t *cfg,
                          telegram_client_t *tg) {
    configure_client_socket(fd);

    req_ctx_t ctx = {0};
    ctx.cfg = cfg;
    (void)snprintf(ctx.peer, sizeof(ctx.peer), "%s",
                   (peer != NULL && peer[0] != '\0') ? peer : "unknown");
    uint8_t method = 0;

    if (!read_and_parse_request(fd, &ctx, &method)) {
        send_parse_error_and_drain(fd, ctx.error_status);
    } else {
        dispatch_request(fd, method, &ctx, tg);
    }

    free(ctx.body);
    close(fd);
}

static void *worker_main(void *arg) {
    http_server_t *srv = (http_server_t *)arg;
    telegram_client_t tg;
    if (!telegram_client_init(&tg, srv->cfg)) {
        pthread_mutex_lock(&srv->mu);
        if (srv->available_workers > 0) {
            srv->available_workers--;
        }
        pthread_mutex_unlock(&srv->mu);
        log_error("Failed to initialize Telegram client in worker");
        return NULL;
    }

    pthread_mutex_lock(&srv->mu);
    for (;;) {
        while (srv->queue.count == 0 && !atomic_load(&srv->stopping)) {
            pthread_cond_wait(&srv->not_empty, &srv->mu);
        }
        if (srv->queue.count == 0 && atomic_load(&srv->stopping)) {
            pthread_mutex_unlock(&srv->mu);
            break;
        }
        client_conn_t conn = srv->queue.items[srv->queue.head];
        srv->queue.head = (srv->queue.head + 1) % QUEUE_CAPACITY;
        srv->queue.count--;
        srv->busy_workers++;
        pthread_mutex_unlock(&srv->mu);

        handle_client(conn.fd, conn.peer, srv->cfg, &tg);

        pthread_mutex_lock(&srv->mu);
        srv->busy_workers--;
    }

    telegram_client_destroy(&tg);
    return NULL;
}

static bool set_nonblock_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    int fdflags = fcntl(fd, F_GETFD, 0);
    if (fdflags < 0 || fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) < 0) {
        return false;
    }
    return true;
}

static int create_listen_socket(const char *bind_addr, uint16_t port,
                                uint16_t *out_port) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(bind_addr, port_str, &hints, &res);
    if (gai != 0) {
        /* Fallback to DNS/hostname resolution if bind_addr is not a numeric
         * literal */
        hints.ai_flags = AI_PASSIVE;
        gai = getaddrinfo(bind_addr, port_str, &hints, &res);
    }
    if (gai != 0 || res == NULL) {
        log_error("getaddrinfo(%s, %s): %s", bind_addr, port_str,
                  gai_strerror(gai));
        return -1;
    }

    int listen_fd = -1;
    for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next) {
        int fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int yes = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0 &&
            listen(fd, 128) == 0) {
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            if (getsockname(fd, (struct sockaddr *)&ss, &sslen) == 0) {
                if (ss.ss_family == AF_INET) {
                    *out_port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
                } else if (ss.ss_family == AF_INET6) {
                    *out_port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
                }
            }
            listen_fd = fd;
            break;
        }
        close(fd);
    }

    freeaddrinfo(res);
    if (listen_fd < 0) {
        log_error("Failed to bind/listen on %s:%u: %s", bind_addr,
                  (unsigned)port, strerror(errno));
    }
    return listen_fd;
}

http_server_t *http_server_create(const app_config_t *cfg) {
    if (cfg == NULL) {
        return NULL;
    }

    http_server_t *srv = calloc(1, sizeof(*srv));
    if (srv == NULL) {
        return NULL;
    }
    srv->cfg = cfg;
    srv->listen_fd = -1;
    srv->wake_pipe[0] = -1;
    srv->wake_pipe[1] = -1;
    atomic_init(&srv->stopping, false);

    if (pipe(srv->wake_pipe) != 0 || !set_nonblock_cloexec(srv->wake_pipe[0]) ||
        !set_nonblock_cloexec(srv->wake_pipe[1])) {
        log_error("Failed to create wake pipe");
        http_server_destroy(srv);
        return NULL;
    }

    srv->listen_fd =
        create_listen_socket(cfg->bind_addr, cfg->port, &srv->bound_port);
    if (srv->listen_fd < 0) {
        http_server_destroy(srv);
        return NULL;
    }
    (void)set_nonblock_cloexec(srv->listen_fd);

    if (pthread_mutex_init(&srv->mu, NULL) != 0) {
        http_server_destroy(srv);
        return NULL;
    }
    srv->mu_initialized = true;
    if (pthread_cond_init(&srv->not_empty, NULL) != 0) {
        http_server_destroy(srv);
        return NULL;
    }
    srv->cond_initialized = true;

    srv->threads = (pthread_t *)calloc(cfg->worker_threads, sizeof(pthread_t));
    if (srv->threads == NULL) {
        http_server_destroy(srv);
        return NULL;
    }

    srv->available_workers = cfg->worker_threads;

    for (size_t i = 0; i < cfg->worker_threads; i++) {
        if (pthread_create(&srv->threads[i], NULL, worker_main, srv) != 0) {
            http_server_signal_stop(srv);
            http_server_destroy(srv);
            return NULL;
        }
        srv->num_threads++;
    }

    return srv;
}

uint16_t http_server_port(const http_server_t *srv) {
    return srv != NULL ? srv->bound_port : 0;
}

void http_server_run(http_server_t *srv) {
    if (srv == NULL) {
        return;
    }

    log_info("Telegram notifier listening on %s:%u", srv->cfg->bind_addr,
             (unsigned)srv->bound_port);

    struct pollfd pfds[2];
    pfds[0].fd = srv->listen_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = srv->wake_pipe[0];
    pfds[1].events = POLLIN;

    while (!atomic_load(&srv->stopping)) {
        int rc = poll(pfds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("poll failed: %s", strerror(errno));
            break;
        }

        if (pfds[1].revents & POLLIN) {
            char drain[32];
            while (read(srv->wake_pipe[0], drain, sizeof(drain)) > 0) {
            }
            break;
        }

        if (pfds[0].revents & POLLIN) {
            for (;;) {
                struct sockaddr_storage peer_addr;
                memset(&peer_addr, 0, sizeof(peer_addr));
                socklen_t peer_len = sizeof(peer_addr);
                int client_fd = accept(
                    srv->listen_fd, (struct sockaddr *)&peer_addr, &peer_len);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    log_error("accept failed: %s", strerror(errno));
                    break;
                }

                char peer[MAX_PEER_ADDR_BYTES];
                http_format_peer_addr((const struct sockaddr *)&peer_addr,
                                      peer_len, peer, sizeof(peer));

                pthread_mutex_lock(&srv->mu);
                size_t in_flight = srv->busy_workers + srv->queue.count;
                size_t available = srv->available_workers;
                if (in_flight >= available ||
                    srv->queue.count >= QUEUE_CAPACITY) {
                    pthread_mutex_unlock(&srv->mu);
                    log_error("All workers busy (%zu/%zu), rejecting client "
                              "with 429 (from=%s)",
                              in_flight, available, peer);
                    (void)set_blocking_cloexec(client_fd);
                    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
                    (void)setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv,
                                     sizeof(tv));
                    send_json_error(client_fd, 429,
                                    "Too many requests, try again later");
                    (void)shutdown(client_fd, SHUT_WR);
                    (void)set_nonblock_cloexec(client_fd);
                    char drain_buf[4096];
                    for (int i = 0; i < 4; i++) {
                        if (recv(client_fd, drain_buf, sizeof(drain_buf), 0) <=
                            0) {
                            break;
                        }
                    }
                    close(client_fd);
                    continue;
                }
                client_conn_t *slot = &srv->queue.items[srv->queue.tail];
                slot->fd = client_fd;
                memcpy(slot->peer, peer, sizeof(slot->peer));
                srv->queue.tail = (srv->queue.tail + 1) % QUEUE_CAPACITY;
                srv->queue.count++;
                pthread_cond_signal(&srv->not_empty);
                pthread_mutex_unlock(&srv->mu);
            }
        }
    }
}

void http_server_signal_stop(http_server_t *srv) {
    if (srv == NULL) {
        return;
    }
    atomic_store(&srv->stopping, true);
    if (srv->wake_pipe[1] >= 0) {
        char b = 1;
        ssize_t ignored = write(srv->wake_pipe[1], &b, 1);
        (void)ignored;
    }
}

void http_server_destroy(http_server_t *srv) {
    if (srv == NULL) {
        return;
    }

    http_server_signal_stop(srv);

    if (srv->threads != NULL) {
        pthread_mutex_lock(&srv->mu);
        pthread_cond_broadcast(&srv->not_empty);
        pthread_mutex_unlock(&srv->mu);

        for (size_t i = 0; i < srv->num_threads; i++) {
            pthread_join(srv->threads[i], NULL);
        }
        free((void *)srv->threads);
        srv->threads = NULL;
    }

    while (srv->queue.count > 0) {
        close(srv->queue.items[srv->queue.head].fd);
        srv->queue.head = (srv->queue.head + 1) % QUEUE_CAPACITY;
        srv->queue.count--;
    }

    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
    }
    if (srv->wake_pipe[0] >= 0) {
        close(srv->wake_pipe[0]);
    }
    if (srv->wake_pipe[1] >= 0) {
        close(srv->wake_pipe[1]);
    }

    if (srv->cond_initialized) {
        pthread_cond_destroy(&srv->not_empty);
    }
    if (srv->mu_initialized) {
        pthread_mutex_destroy(&srv->mu);
    }
    free(srv);
}
