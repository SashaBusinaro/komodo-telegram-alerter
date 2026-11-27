#include "alert.h"

#include <ctype.h>
#include <json-c/json.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err_buf, size_t err_buf_len, const char *msg) {
    if (err_buf != NULL && err_buf_len > 0) {
        (void)snprintf(err_buf, err_buf_len, "%s", msg);
    }
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
static void set_err_fmt(char *err_buf, size_t err_buf_len, const char *fmt,
                        ...) {
    if (err_buf == NULL || err_buf_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err_buf, err_buf_len, fmt, ap);
    va_end(ap);
}

bool validate_telegram_token(const char *token) {
    if (token == NULL) {
        return false;
    }

    size_t len = strlen(token);
    if (len == 0 || len > 128) {
        return false;
    }

    const char *colon = strchr(token, ':');
    if (colon == NULL || colon == token) {
        return false;
    }

    /* Bot ID prefix: one or more ASCII digits */
    for (const char *p = token; p < colon; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }

    /* Secret suffix: one or more [A-Za-z0-9_-] */
    const char *secret = colon + 1;
    if (*secret == '\0') {
        return false;
    }
    for (const char *p = secret; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }

    return true;
}

bool validate_telegram_chat_id(const char *chat_id) {
    if (chat_id == NULL) {
        return false;
    }

    size_t len = strlen(chat_id);
    if (len == 0 || len > 64) {
        return false;
    }

    if (chat_id[0] == '@') {
        /* Channel username: ^@[A-Za-z0-9_]{5,}$ */
        if (len < 6) {
            return false;
        }
        for (size_t i = 1; i < len; i++) {
            unsigned char c = (unsigned char)chat_id[i];
            bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '_';
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    /* Numeric chat/channel ID: ^-?[0-9]+$ */
    size_t start = (chat_id[0] == '-') ? 1 : 0;
    if (start >= len) {
        return false;
    }
    for (size_t i = start; i < len; i++) {
        if (chat_id[i] < '0' || chat_id[i] > '9') {
            return false;
        }
    }
    return true;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} strbuf_t;

static bool strbuf_reserve(strbuf_t *sb, size_t extra) {
    if (extra > SIZE_MAX - sb->len - 1) {
        return false;
    }
    size_t needed = sb->len + extra + 1;
    if (needed <= sb->cap) {
        return true;
    }
    size_t new_cap = sb->cap == 0 ? 512 : sb->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    char *next = realloc(sb->data, new_cap);
    if (next == NULL) {
        return false;
    }
    sb->data = next;
    sb->cap = new_cap;
    return true;
}

static bool strbuf_append_n(strbuf_t *sb, const char *s, size_t n) {
    if (!strbuf_reserve(sb, n)) {
        return false;
    }
    if (n > 0) {
        memcpy(sb->data + sb->len, s, n);
        sb->len += n;
    }
    sb->data[sb->len] = '\0';
    return true;
}

static bool strbuf_append(strbuf_t *sb, const char *s) {
    return strbuf_append_n(sb, s, strlen(s));
}

static bool strbuf_append_indent(strbuf_t *sb, int depth) {
    for (int i = 0; i < depth; i++) {
        if (!strbuf_append_n(sb, "  ", 2)) {
            return false;
        }
    }
    return true;
}

static bool append_json_key(strbuf_t *sb, const char *key) {
    struct json_object *tmp = json_object_new_string(key);
    if (tmp == NULL) {
        return false;
    }
    size_t serialized_len = 0;
    const char *serialized = json_object_to_json_string_length(
        tmp, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE,
        &serialized_len);
    bool ok =
        (serialized != NULL) && strbuf_append_n(sb, serialized, serialized_len);
    json_object_put(tmp);
    return ok;
}

#define JSON_MAX_PRETTY_DEPTH 64

typedef struct {
    struct json_object *container;
    struct json_object_iterator obj_it;
    struct json_object_iterator obj_end;
    size_t arr_idx;
    size_t arr_len;
    bool is_object;
    bool first;
} json_fmt_frame_t;

static bool append_json_scalar(strbuf_t *sb, struct json_object *obj) {
    if (obj == NULL || json_object_is_type(obj, json_type_null)) {
        return strbuf_append_n(sb, "null", 4);
    }
    size_t scalar_len = 0;
    const char *scalar = json_object_to_json_string_length(
        obj, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE,
        &scalar_len);
    if (scalar == NULL) {
        return false;
    }
    return strbuf_append_n(sb, scalar, scalar_len);
}

static bool append_json_pretty_2space(strbuf_t *sb, struct json_object *obj) {
    json_fmt_frame_t stack[JSON_MAX_PRETTY_DEPTH];
    int sp = 0;
    struct json_object *cur = obj;

    for (;;) {
        if (cur == NULL || json_object_is_type(cur, json_type_null)) {
            if (!append_json_scalar(sb, cur)) {
                return false;
            }
        } else {
            enum json_type type = json_object_get_type(cur);
            if (type == json_type_object) {
                if (json_object_object_length(cur) == 0) {
                    if (!strbuf_append_n(sb, "{}", 2)) {
                        return false;
                    }
                } else {
                    if (sp >= JSON_MAX_PRETTY_DEPTH ||
                        !strbuf_append_n(sb, "{\n", 2)) {
                        return false;
                    }
                    memset(&stack[sp], 0, sizeof(stack[sp]));
                    stack[sp].is_object = true;
                    stack[sp].container = cur;
                    stack[sp].obj_it = json_object_iter_begin(cur);
                    stack[sp].obj_end = json_object_iter_end(cur);
                    stack[sp].first = true;
                    sp++;
                }
            } else if (type == json_type_array) {
                size_t len = json_object_array_length(cur);
                if (len == 0) {
                    if (!strbuf_append_n(sb, "[]", 2)) {
                        return false;
                    }
                } else {
                    if (sp >= JSON_MAX_PRETTY_DEPTH ||
                        !strbuf_append_n(sb, "[\n", 2)) {
                        return false;
                    }
                    memset(&stack[sp], 0, sizeof(stack[sp]));
                    stack[sp].is_object = false;
                    stack[sp].container = cur;
                    stack[sp].arr_len = len;
                    stack[sp].first = true;
                    sp++;
                }
            } else if (!append_json_scalar(sb, cur)) {
                return false;
            }
        }

        bool have_next = false;
        while (sp > 0) {
            json_fmt_frame_t *top = &stack[sp - 1];
            if (top->is_object) {
                if (!json_object_iter_equal(&top->obj_it, &top->obj_end)) {
                    if (!top->first && !strbuf_append_n(sb, ",\n", 2)) {
                        return false;
                    }
                    top->first = false;
                    const char *key = json_object_iter_peek_name(&top->obj_it);
                    struct json_object *val =
                        json_object_iter_peek_value(&top->obj_it);
                    json_object_iter_next(&top->obj_it);
                    if (!strbuf_append_indent(sb, sp) ||
                        !append_json_key(sb, key) ||
                        !strbuf_append_n(sb, ": ", 2)) {
                        return false;
                    }
                    cur = val;
                    have_next = true;
                    break;
                }
                sp--;
                if (!strbuf_append_n(sb, "\n", 1) ||
                    !strbuf_append_indent(sb, sp) ||
                    !strbuf_append_n(sb, "}", 1)) {
                    return false;
                }
            } else {
                if (top->arr_idx < top->arr_len) {
                    if (!top->first && !strbuf_append_n(sb, ",\n", 2)) {
                        return false;
                    }
                    top->first = false;
                    if (!strbuf_append_indent(sb, sp)) {
                        return false;
                    }
                    cur = json_object_array_get_idx(top->container,
                                                    top->arr_idx++);
                    have_next = true;
                    break;
                }
                sp--;
                if (!strbuf_append_n(sb, "\n", 1) ||
                    !strbuf_append_indent(sb, sp) ||
                    !strbuf_append_n(sb, "]", 1)) {
                    return false;
                }
            }
        }

        if (!have_next) {
            return true;
        }
    }
}

static bool is_js_truthy(struct json_object *obj) {
    if (obj == NULL) {
        return false;
    }
    switch (json_object_get_type(obj)) {
    case json_type_null:
        return false;
    case json_type_boolean:
        return json_object_get_boolean(obj) != 0;
    case json_type_int:
        return json_object_get_int64(obj) != 0 ||
               json_object_get_uint64(obj) != 0;
    case json_type_double: {
        double d = json_object_get_double(obj);
        return d != 0.0 && !isnan(d);
    }
    case json_type_string:
        return json_object_get_string_len(obj) > 0;
    case json_type_object:
    case json_type_array:
        return true;
    }
    return false;
}

static struct json_object *get_object_field(struct json_object *obj,
                                            const char *key) {
    if (obj == NULL || !json_object_is_type(obj, json_type_object)) {
        return NULL;
    }
    struct json_object *val = NULL;
    if (!json_object_object_get_ex(obj, key, &val)) {
        return NULL;
    }
    return val;
}

static const char *get_truthy_as_string(struct json_object *obj,
                                        const char *fallback) {
    if (!is_js_truthy(obj)) {
        return fallback;
    }
    const char *s = json_object_get_string(obj);
    return (s != NULL && s[0] != '\0') ? s : fallback;
}

static bool equals_ascii_icase(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const char *level_to_emoji(const char *level) {
    if (equals_ascii_icase(level, "CRITICAL")) {
        return "\xF0\x9F\x94\xB4"; /* 🔴 */
    }
    if (equals_ascii_icase(level, "ERROR")) {
        return "\xF0\x9F\x9A\xA8"; /* 🚨 */
    }
    if (equals_ascii_icase(level, "WARNING")) {
        return "\xE2\x9A\xA0\xEF\xB8\x8F"; /* ⚠️ */
    }
    if (equals_ascii_icase(level, "INFO")) {
        return "\xE2\x84\xB9\xEF\xB8\x8F"; /* ℹ️ */
    }
    if (equals_ascii_icase(level, "OK")) {
        return "\xE2\x9C\x85"; /* ✅ */
    }
    return "\xE2\x84\xB9\xEF\xB8\x8F"; /* ℹ️ */
}

char *format_komodo_alert(const char *json_body, size_t body_len, char *err_buf,
                          size_t err_buf_len) {
    if (json_body == NULL || body_len == 0) {
        set_err(err_buf, err_buf_len, "empty request body (0 bytes)");
        return NULL;
    }
    if (body_len > (size_t)INT_MAX) {
        set_err_fmt(err_buf, err_buf_len, "request body too large (%zu bytes)",
                    body_len);
        return NULL;
    }

    struct json_tokener *tok = json_tokener_new();
    if (tok == NULL) {
        set_err(err_buf, err_buf_len, "out of memory allocating JSON tokener");
        return NULL;
    }
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);

    struct json_object *root =
        json_tokener_parse_ex(tok, json_body, (int)body_len);
    enum json_tokener_error jerr = json_tokener_get_error(tok);
    size_t parsed_end = json_tokener_get_parse_end(tok);
    json_tokener_free(tok);

    if (jerr != json_tokener_success || root == NULL) {
        if (root != NULL) {
            json_object_put(root);
        }
        set_err_fmt(err_buf, err_buf_len, "%s at byte offset %zu of %zu",
                    json_tokener_error_desc(jerr), parsed_end, body_len);
        return NULL;
    }

    for (size_t i = parsed_end; i < body_len; i++) {
        unsigned char c = (unsigned char)json_body[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            json_object_put(root);
            set_err_fmt(err_buf, err_buf_len,
                        "unexpected trailing data at byte offset %zu of %zu", i,
                        body_len);
            return NULL;
        }
    }

    if (!json_object_is_type(root, json_type_object)) {
        const char *type_name = json_type_to_name(json_object_get_type(root));
        json_object_put(root);
        set_err_fmt(err_buf, err_buf_len,
                    "expected JSON object, got %s (%zu bytes)",
                    type_name != NULL ? type_name : "unknown", body_len);
        return NULL;
    }

    struct json_object *data_obj = get_object_field(root, "data");
    struct json_object *nested_data = get_object_field(data_obj, "data");
    struct json_object *target_obj = get_object_field(root, "target");

    const char *level =
        get_truthy_as_string(get_object_field(root, "level"), "Unknown");
    const char *type =
        get_truthy_as_string(get_object_field(data_obj, "type"), "Unknown");
    const char *name =
        get_truthy_as_string(get_object_field(nested_data, "name"), "Unnamed");
    const char *target_type = get_truthy_as_string(
        get_object_field(target_obj, "type"), "Unknown Target Type");
    const char *resolved = is_js_truthy(get_object_field(root, "resolved"))
                               ? "\xE2\x9C\x85"  /* ✅ */
                               : "\xE2\x9D\x8C"; /* ❌ */
    const char *emoji = level_to_emoji(level);

    struct json_object *fallback_info = NULL;
    struct json_object *alert_info = nested_data;
    if (!is_js_truthy(alert_info)) {
        fallback_info = json_object_new_object();
        if (fallback_info == NULL) {
            json_object_put(root);
            set_err(err_buf, err_buf_len, "Out of memory");
            return NULL;
        }
        struct json_object *info_str =
            json_object_new_string("No alert data available");
        if (info_str == NULL ||
            json_object_object_add(fallback_info, "info", info_str) != 0) {
            if (info_str != NULL) {
                json_object_put(info_str);
            }
            json_object_put(fallback_info);
            json_object_put(root);
            set_err(err_buf, err_buf_len, "Out of memory");
            return NULL;
        }
        alert_info = fallback_info;
    }

    strbuf_t sb = {0};
    bool ok = strbuf_append(&sb, emoji) && strbuf_append(&sb, " ") &&
              strbuf_append(&sb, level) && strbuf_append(&sb, " - ") &&
              strbuf_append(&sb, type) && strbuf_append(&sb, "\n*Name*: ") &&
              strbuf_append(&sb, name) && strbuf_append(&sb, " (") &&
              strbuf_append(&sb, target_type) &&
              strbuf_append(&sb, ")\n*Resolved*: ") &&
              strbuf_append(&sb, resolved) &&
              strbuf_append(&sb, "\n*Data*: ") &&
              append_json_pretty_2space(&sb, alert_info);

    if (fallback_info != NULL) {
        json_object_put(fallback_info);
    }
    json_object_put(root);

    if (!ok) {
        free(sb.data);
        set_err(err_buf, err_buf_len,
                "failed to format alert data (nesting depth exceeded or out of "
                "memory)");
        return NULL;
    }

    return sb.data;
}
