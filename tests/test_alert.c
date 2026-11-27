#include "alert.h"
#include "http_server.h"
#include "logging.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #cond);                                          \
            abort();                                                           \
        }                                                                      \
    } while (0)

static void test_token_validation(void) {
    TEST_ASSERT(validate_telegram_token("123456789:ABCdefGHI_123-xyz"));
    TEST_ASSERT(validate_telegram_token("1:a"));

    TEST_ASSERT(!validate_telegram_token(NULL));
    TEST_ASSERT(!validate_telegram_token(""));
    TEST_ASSERT(!validate_telegram_token(":ABC"));
    TEST_ASSERT(!validate_telegram_token("123:"));
    TEST_ASSERT(!validate_telegram_token("abc:DEF"));
    TEST_ASSERT(!validate_telegram_token("123:ABC/../../etc/passwd"));
    TEST_ASSERT(!validate_telegram_token("123:ABC?foo=bar"));
    TEST_ASSERT(!validate_telegram_token("123:ABC#frag"));
    TEST_ASSERT(!validate_telegram_token("123:ABC DEF"));
    TEST_ASSERT(!validate_telegram_token("123:ABC@evil.com"));
}

static void test_chat_id_validation(void) {
    TEST_ASSERT(validate_telegram_chat_id("123456789"));
    TEST_ASSERT(validate_telegram_chat_id("-1001234567890"));
    TEST_ASSERT(validate_telegram_chat_id("@my_channel_123"));

    TEST_ASSERT(!validate_telegram_chat_id(NULL));
    TEST_ASSERT(!validate_telegram_chat_id(""));
    TEST_ASSERT(!validate_telegram_chat_id("-"));
    TEST_ASSERT(!validate_telegram_chat_id("123abc"));
    TEST_ASSERT(
        !validate_telegram_chat_id("@abcd")); /* too short (<5 after @) */
    TEST_ASSERT(!validate_telegram_chat_id("@bad-name"));
    TEST_ASSERT(!validate_telegram_chat_id("123 456"));
}

static void test_query_param_parsing(void) {
    char buf[128];
    bool found = false;

    const char *q1 = "token=12345%3AABC_def&chat_id=-100999";
    TEST_ASSERT(http_query_get_param(q1, strlen(q1), "token", buf, sizeof(buf),
                                     &found));
    TEST_ASSERT(found && strcmp(buf, "12345:ABC_def") == 0);

    TEST_ASSERT(http_query_get_param(q1, strlen(q1), "chat_id", buf,
                                     sizeof(buf), &found));
    TEST_ASSERT(found && strcmp(buf, "-100999") == 0);

    TEST_ASSERT(http_query_get_param(q1, strlen(q1), "missing", buf,
                                     sizeof(buf), &found));
    TEST_ASSERT(!found);

    /* NUL byte injection via %00 must fail */
    const char *q_nul = "token=123%00456";
    TEST_ASSERT(!http_query_get_param(q_nul, strlen(q_nul), "token", buf,
                                      sizeof(buf), &found));

    /* Malformed percent encoding must fail */
    const char *q_bad = "token=123%ZZ";
    TEST_ASSERT(!http_query_get_param(q_bad, strlen(q_bad), "token", buf,
                                      sizeof(buf), &found));

    const char *q_trunc = "token=123%2";
    TEST_ASSERT(!http_query_get_param(q_trunc, strlen(q_trunc), "token", buf,
                                      sizeof(buf), &found));

    /* Empty value is allowed by parser (checked separately by caller) */
    const char *q_empty = "token=&chat_id=123";
    TEST_ASSERT(http_query_get_param(q_empty, strlen(q_empty), "token", buf,
                                     sizeof(buf), &found));
    TEST_ASSERT(found && strcmp(buf, "") == 0);
}

static void test_full_komodo_alert_parity(void) {
    const char *payload =
        "{"
        "  \"level\": \"OK\","
        "  \"resolved\": true,"
        "  \"target\": { \"type\": \"Stack\" },"
        "  \"data\": {"
        "    \"type\": \"StackAutoUpdated\","
        "    \"data\": {"
        "      \"id\": \"685db922720baf6840daffca\","
        "      \"name\": \"komodo\","
        "      \"images\": [\"ghcr.io/moghtech/komodo:latest\"],"
        "      \"empty_obj\": {},"
        "      \"empty_arr\": []"
        "    }"
        "  }"
        "}";

    const char *expected = "\xE2\x9C\x85 OK - StackAutoUpdated\n"
                           "*Name*: komodo (Stack)\n"
                           "*Resolved*: \xE2\x9C\x85\n"
                           "*Data*: {\n"
                           "  \"id\": \"685db922720baf6840daffca\",\n"
                           "  \"name\": \"komodo\",\n"
                           "  \"images\": [\n"
                           "    \"ghcr.io/moghtech/komodo:latest\"\n"
                           "  ],\n"
                           "  \"empty_obj\": {},\n"
                           "  \"empty_arr\": []\n"
                           "}";

    char err[128] = {0};
    char *msg = format_komodo_alert(payload, strlen(payload), err, sizeof(err));
    TEST_ASSERT(msg != NULL);
    if (strcmp(msg, expected) != 0) {
        fprintf(stderr, "Expected:\n%s\n\nActual:\n%s\n", expected, msg);
    }
    TEST_ASSERT(strcmp(msg, expected) == 0);
    free(msg);
}

static void test_level_emojis_and_defaults(void) {
    char err[128] = {0};

    const char *crit = "{\"level\":\"critical\",\"resolved\":false}";
    char *msg = format_komodo_alert(crit, strlen(crit), err, sizeof(err));
    TEST_ASSERT(msg != NULL);
    const char *expected_crit = "\xF0\x9F\x94\xB4 critical - Unknown\n"
                                "*Name*: Unnamed (Unknown Target Type)\n"
                                "*Resolved*: \xE2\x9D\x8C\n"
                                "*Data*: {\n"
                                "  \"info\": \"No alert data available\"\n"
                                "}";
    TEST_ASSERT(strcmp(msg, expected_crit) == 0);
    free(msg);

    const char *err_json = "{\"level\":\"Error\"}";
    msg = format_komodo_alert(err_json, strlen(err_json), err, sizeof(err));
    TEST_ASSERT(msg != NULL);
    TEST_ASSERT(strncmp(msg, "\xF0\x9F\x9A\xA8 Error - Unknown\n",
                        strlen("\xF0\x9F\x9A\xA8 Error - Unknown\n")) == 0);
    free(msg);

    const char *warn_json = "{\"level\":\"WARNING\"}";
    msg = format_komodo_alert(warn_json, strlen(warn_json), err, sizeof(err));
    TEST_ASSERT(msg != NULL);
    TEST_ASSERT(
        strncmp(msg, "\xE2\x9A\xA0\xEF\xB8\x8F WARNING - Unknown\n",
                strlen("\xE2\x9A\xA0\xEF\xB8\x8F WARNING - Unknown\n")) == 0);
    free(msg);

    const char *info_json = "{\"level\":\"info\"}";
    msg = format_komodo_alert(info_json, strlen(info_json), err, sizeof(err));
    TEST_ASSERT(msg != NULL);
    TEST_ASSERT(strncmp(msg, "\xE2\x84\xB9\xEF\xB8\x8F info - Unknown\n",
                        strlen("\xE2\x84\xB9\xEF\xB8\x8F info - Unknown\n")) ==
                0);
    free(msg);

    const char *other_json = "{\"level\":\"custom\"}";
    msg = format_komodo_alert(other_json, strlen(other_json), err, sizeof(err));
    TEST_ASSERT(msg != NULL);
    TEST_ASSERT(
        strncmp(msg, "\xE2\x84\xB9\xEF\xB8\x8F custom - Unknown\n",
                strlen("\xE2\x84\xB9\xEF\xB8\x8F custom - Unknown\n")) == 0);
    free(msg);

    const char *empty_obj = "{}";
    msg = format_komodo_alert(empty_obj, strlen(empty_obj), err, sizeof(err));
    TEST_ASSERT(msg != NULL);
    TEST_ASSERT(
        strncmp(msg, "\xE2\x84\xB9\xEF\xB8\x8F Unknown - Unknown\n",
                strlen("\xE2\x84\xB9\xEF\xB8\x8F Unknown - Unknown\n")) == 0);
    free(msg);
}

static void test_invalid_json_inputs(void) {
    char err[256] = {0};

    TEST_ASSERT(format_komodo_alert(NULL, 0, err, sizeof(err)) == NULL);
    TEST_ASSERT(strstr(err, "empty request body") != NULL);

    TEST_ASSERT(format_komodo_alert("", 0, err, sizeof(err)) == NULL);
    TEST_ASSERT(strstr(err, "empty request body") != NULL);

    TEST_ASSERT(format_komodo_alert("{bad", 4, err, sizeof(err)) == NULL);
    TEST_ASSERT(strstr(err, "at byte offset") != NULL);

    TEST_ASSERT(format_komodo_alert("[]", 2, err, sizeof(err)) == NULL);
    TEST_ASSERT(strstr(err, "expected JSON object, got array") != NULL);

    TEST_ASSERT(format_komodo_alert("\"str\"", 5, err, sizeof(err)) == NULL);
    TEST_ASSERT(strstr(err, "expected JSON object, got string") != NULL);

    TEST_ASSERT(format_komodo_alert("{}{}", 4, err, sizeof(err)) == NULL);
    TEST_ASSERT(strstr(err, "at byte offset 2") != NULL);

    TEST_ASSERT(format_komodo_alert("{} garbage", 10, err, sizeof(err)) ==
                NULL);
    TEST_ASSERT(strstr(err, "at byte offset 3") != NULL);
}

static void test_logging_preview_and_levels(void) {
    char out[64];
    const char *raw = "a\nb\rc\td\"e\\f\x01\x7fg";
    log_format_preview(raw, strlen(raw), out, sizeof(out));
    TEST_ASSERT(strcmp(out, "a\\nb\\rc\\td\\\"e\\\\f??g") == 0);

    char trunc[6];
    log_format_preview("abcdef", 6, trunc, sizeof(trunc));
    TEST_ASSERT(strcmp(trunc, "ab...") == 0);

    /* Ensure truncation never leaves a trailing odd backslash before "..." */
    char bs_edge[6];
    log_format_preview("a\nbcdef", 7, bs_edge, sizeof(bs_edge));
    TEST_ASSERT(strcmp(bs_edge, "a...") == 0);

    log_set_level(LOG_LEVEL_DEBUG);
    TEST_ASSERT(log_get_level() == LOG_LEVEL_DEBUG);
    log_set_level(LOG_LEVEL_ERROR);
    TEST_ASSERT(log_get_level() == LOG_LEVEL_ERROR);
    log_set_level(LOG_LEVEL_INFO);
    TEST_ASSERT(log_get_level() == LOG_LEVEL_INFO);
}

static void test_format_peer_addr(void) {
    char buf[64];

    struct sockaddr_in sin = {0};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(8080);
    TEST_ASSERT(inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr) == 1);
    http_format_peer_addr((const struct sockaddr *)&sin, sizeof(sin), buf,
                          sizeof(buf));
    TEST_ASSERT(strcmp(buf, "127.0.0.1:8080") == 0);

    struct sockaddr_in6 sin6 = {0};
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(443);
    TEST_ASSERT(inet_pton(AF_INET6, "::1", &sin6.sin6_addr) == 1);
    http_format_peer_addr((const struct sockaddr *)&sin6, sizeof(sin6), buf,
                          sizeof(buf));
    TEST_ASSERT(strcmp(buf, "[::1]:443") == 0);

    struct sockaddr_in6 mapped = {0};
    mapped.sin6_family = AF_INET6;
    mapped.sin6_port = htons(54321);
    TEST_ASSERT(inet_pton(AF_INET6, "::ffff:192.0.2.15", &mapped.sin6_addr) ==
                1);
    http_format_peer_addr((const struct sockaddr *)&mapped, sizeof(mapped), buf,
                          sizeof(buf));
    TEST_ASSERT(strcmp(buf, "192.0.2.15:54321") == 0);

    http_format_peer_addr(NULL, 0, buf, sizeof(buf));
    TEST_ASSERT(strcmp(buf, "unknown") == 0);

    struct sockaddr unspec = {0};
    unspec.sa_family = AF_UNSPEC;
    http_format_peer_addr(&unspec, sizeof(unspec), buf, sizeof(buf));
    TEST_ASSERT(strcmp(buf, "unknown") == 0);

    http_format_peer_addr((const struct sockaddr *)&sin, sizeof(sin) - 1, buf,
                          sizeof(buf));
    TEST_ASSERT(strcmp(buf, "unknown") == 0);
}

static void test_validation_boundaries(void) {
    /* Token max length: 128 bytes allowed, 129 rejected */
    char tok128[129];
    tok128[0] = '1';
    tok128[1] = ':';
    memset(tok128 + 2, 'a', 126);
    tok128[128] = '\0';
    TEST_ASSERT(validate_telegram_token(tok128));

    char tok129[130];
    tok129[0] = '1';
    tok129[1] = ':';
    memset(tok129 + 2, 'a', 127);
    tok129[129] = '\0';
    TEST_ASSERT(!validate_telegram_token(tok129));
    TEST_ASSERT(!validate_telegram_token("1:\x80"));

    /* Chat ID max length: 64 bytes allowed, 65 rejected */
    char cid64[65];
    memset(cid64, '9', 64);
    cid64[64] = '\0';
    TEST_ASSERT(validate_telegram_chat_id(cid64));

    cid64[0] = '-';
    TEST_ASSERT(validate_telegram_chat_id(cid64));

    cid64[0] = '@';
    memset(cid64 + 1, 'a', 63);
    TEST_ASSERT(validate_telegram_chat_id(cid64));

    char cid65[66];
    memset(cid65, '9', 65);
    cid65[65] = '\0';
    TEST_ASSERT(!validate_telegram_chat_id(cid65));

    TEST_ASSERT(validate_telegram_chat_id("@abcde"));
    TEST_ASSERT(!validate_telegram_chat_id("@abcd"));
}

static void test_query_param_boundaries(void) {
    char exact[6]; /* fits "hello" + NUL */
    char small[5]; /* 1 byte too short for "hello" */
    bool found = false;

    const char *q_fit = "k=hello&other=1";
    TEST_ASSERT(http_query_get_param(q_fit, strlen(q_fit), "k", exact,
                                     sizeof(exact), &found));
    TEST_ASSERT(found && strcmp(exact, "hello") == 0);
    TEST_ASSERT(!http_query_get_param(q_fit, strlen(q_fit), "k", small,
                                      sizeof(small), &found));

    /* Duplicate keys (last wins), '+' space decoding, trailing '&', bare key */
    char buf[64];
    const char *q_misc = "flag&k=first+val&k=second%20%41&";
    TEST_ASSERT(http_query_get_param(q_misc, strlen(q_misc), "flag", buf,
                                     sizeof(buf), &found));
    TEST_ASSERT(found && strcmp(buf, "") == 0);
    TEST_ASSERT(http_query_get_param(q_misc, strlen(q_misc), "k", buf,
                                     sizeof(buf), &found));
    TEST_ASSERT(found && strcmp(buf, "second A") == 0);

    /* Empty and NULL queries */
    TEST_ASSERT(http_query_get_param("", 0, "k", buf, sizeof(buf), &found));
    TEST_ASSERT(!found);
    TEST_ASSERT(http_query_get_param(NULL, 0, "k", buf, sizeof(buf), &found));
    TEST_ASSERT(!found);
}

static void test_deep_and_large_json_formatting(void) {
    /* Depth 24 nested JSON (within json-c's default parser limit of 32) */
    char deep_json[1024];
    size_t pos = 0;
    const int depth = 24;
    pos += (size_t)snprintf(deep_json + pos, sizeof(deep_json) - pos,
                            "{\"level\":\"INFO\",\"data\":{\"type\":\"T\","
                            "\"data\":");
    for (int i = 0; i < depth; i++) {
        deep_json[pos++] = '{';
        pos += (size_t)snprintf(deep_json + pos, sizeof(deep_json) - pos,
                                "\"n\":");
    }
    pos += (size_t)snprintf(deep_json + pos, sizeof(deep_json) - pos, "[1,2]");
    for (int i = 0; i < depth; i++) {
        deep_json[pos++] = '}';
    }
    pos += (size_t)snprintf(deep_json + pos, sizeof(deep_json) - pos, "}}");

    char err[128] = {0};
    char *msg = format_komodo_alert(deep_json, pos, err, sizeof(err));
    TEST_ASSERT(msg != NULL);
    TEST_ASSERT(strstr(msg, "\"n\": {") != NULL);
    TEST_ASSERT(strstr(msg, "1,\n") != NULL);
    free(msg);

    /* Large multi-kilobyte payload exercising repeated strbuf_reserve growth */
    const size_t pad_len = 12000;
    size_t big_cap = pad_len + 512;
    char *big_json = malloc(big_cap);
    TEST_ASSERT(big_json != NULL);
    size_t off = (size_t)snprintf(
        big_json, big_cap,
        "{\"level\":\"WARNING\",\"resolved\":false,\"target\":{\"type\":"
        "\"Host\"},"
        "\"data\":{\"type\":\"DiskFull\",\"data\":{\"name\":\"node-1\","
        "\"\":true,\"num\":-42,\"dbl\":3.5,\"nil\":null,"
        "\"escaped\":\"line1\\n\\t\\\"quoted\\\"\\\\slash\",\"blob\":\"");
    memset(big_json + off, 'Z', pad_len);
    off += pad_len;
    off += (size_t)snprintf(big_json + off, big_cap - off, "\"}}}");

    msg = format_komodo_alert(big_json, off, err, sizeof(err));
    free(big_json);
    TEST_ASSERT(msg != NULL);
    TEST_ASSERT(strlen(msg) > pad_len);
    TEST_ASSERT(strstr(msg, "\"\": true") != NULL);
    TEST_ASSERT(strstr(msg, "\"nil\": null") != NULL);
    TEST_ASSERT(
        strstr(msg, "\"escaped\": \"line1\\n\\t\\\"quoted\\\"\\\\slash\"") !=
        NULL);
    free(msg);
}

int main(void) {
    test_token_validation();
    test_chat_id_validation();
    test_query_param_parsing();
    test_full_komodo_alert_parity();
    test_level_emojis_and_defaults();
    test_invalid_json_inputs();
    test_validation_boundaries();
    test_query_param_boundaries();
    test_deep_and_large_json_formatting();
    test_logging_preview_and_levels();
    test_format_peer_addr();
    printf("All test_alert checks passed!\n");
    return 0;
}
