#include "alert.h"
#include "config.h"
#include "http_server.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            (void)fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, \
                          __LINE__, #cond);                                    \
            abort();                                                           \
        }                                                                      \
    } while (0)

static void *pure_worker_fn(void *arg) {
    int tid = *(const int *)arg;
    const char *valid_json =
        "{\"level\":\"ERROR\",\"resolved\":true,\"target\":{\"type\":\"Stack\"}"
        ","
        "\"data\":{\"type\":\"Deploy\",\"data\":{\"name\":\"app\",\"arr\":[1,2,"
        "{\"k\":\"v\"}]}}}";
    const char *invalid_json = "{\"broken\":";
    const char *query = "token=12345%3AABC_def&chat_id=-100999&extra=a%20b";

    for (int i = 0; i < 500; i++) {
        char err[128] = {0};
        char *msg = format_komodo_alert(valid_json, strlen(valid_json), err,
                                        sizeof(err));
        TEST_ASSERT(msg != NULL);
        TEST_ASSERT(strstr(msg, "app (Stack)") != NULL);
        free(msg);

        TEST_ASSERT(format_komodo_alert(invalid_json, strlen(invalid_json), err,
                                        sizeof(err)) == NULL);

        TEST_ASSERT(validate_telegram_token("123456:ABCdef_123-xyz"));
        TEST_ASSERT(!validate_telegram_token("123456:bad/path"));
        TEST_ASSERT(validate_telegram_chat_id("-1001234567890"));
        TEST_ASSERT(!validate_telegram_chat_id("@bad-name"));

        char val[64];
        bool found = false;
        TEST_ASSERT(http_query_get_param(query, strlen(query),
                                         (i + tid) % 2 == 0 ? "token" : "extra",
                                         val, sizeof(val), &found));
        TEST_ASSERT(found && val[0] != '\0');
    }
    return NULL;
}

static void test_concurrent_pure_functions(void) {
    enum { NUM_THREADS = 8 };
    pthread_t threads[NUM_THREADS];
    int tids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        tids[i] = i;
        TEST_ASSERT(
            pthread_create(&threads[i], NULL, pure_worker_fn, &tids[i]) == 0);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0);
    }
}

static void test_server_create_failure_cleanup(void) {
    app_config_t cfg = {0};
    (void)snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "999.999.999.999");
    (void)snprintf(cfg.telegram_api_url, sizeof(cfg.telegram_api_url),
                   "http://127.0.0.1:1");
    cfg.allow_insecure_http = true;
    cfg.port = 0;
    cfg.max_body_bytes = 4096;
    cfg.worker_threads = 2;
    cfg.connect_timeout_ms = 500;
    cfg.request_timeout_ms = 1000;

    TEST_ASSERT(http_server_create(&cfg) == NULL);
}

typedef struct {
    http_server_t *srv;
} server_thread_ctx_t;

static void *server_thread_fn(void *arg) {
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;
    http_server_run(ctx->srv);
    return NULL;
}

typedef struct {
    uint16_t port;
    int thread_id;
    atomic_int *completed;
    atomic_bool *stop_flag;
} client_thread_ctx_t;

static bool send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static int connect_local(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *client_thread_fn(void *arg) {
    client_thread_ctx_t *ctx = (client_thread_ctx_t *)arg;
    char req_buf[9216];
    char resp_buf[1024];

    for (int iter = 0; iter < 200 && !atomic_load(ctx->stop_flag); iter++) {
        int fd = connect_local(ctx->port);
        if (fd < 0) {
            if (atomic_load(ctx->stop_flag)) {
                break;
            }
            usleep(1000);
            continue;
        }

        int kind = (iter + ctx->thread_id) % 6;
        bool sent_ok = false;

        if (kind == 0) {
            const char *req =
                "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: "
                "close\r\n\r\n";
            sent_ok = send_all(fd, req, strlen(req));
        } else if (kind == 1) {
            const char *req =
                "POST /alert?token=bad&chat_id=1 HTTP/1.1\r\nHost: "
                "localhost\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
            sent_ok = send_all(fd, req, strlen(req));
        } else if (kind == 2) {
            size_t off =
                (size_t)snprintf(req_buf, sizeof(req_buf), "GET /health?p=");
            memset(req_buf + off, 'a', 2200);
            off += 2200;
            off += (size_t)snprintf(req_buf + off, sizeof(req_buf) - off,
                                    " HTTP/1.1\r\nHost: localhost\r\n\r\n");
            sent_ok = send_all(fd, req_buf, off);
        } else if (kind == 3) {
            size_t off = (size_t)snprintf(
                req_buf, sizeof(req_buf),
                "GET /health HTTP/1.1\r\nHost: localhost\r\nX-Big: ");
            memset(req_buf + off, 'b', 8300);
            off += 8300;
            off += (size_t)snprintf(req_buf + off, sizeof(req_buf) - off,
                                    "\r\n\r\n");
            sent_ok = send_all(fd, req_buf, off);
        } else if (kind == 4) {
            const char *part1 = "GET /health ";
            const char *part2 = "HTTP/1.1\r\nHost: localhost\r\n\r\n";
            sent_ok = send_all(fd, part1, strlen(part1));
            if (sent_ok) {
                usleep(500);
                sent_ok = send_all(fd, part2, strlen(part2));
            }
        } else {
            const char *req = "GARBAGE\r\n\r\n";
            sent_ok = send_all(fd, req, strlen(req));
        }

        if (sent_ok) {
            ssize_t total = 0;
            for (;;) {
                ssize_t r = recv(fd, resp_buf, sizeof(resp_buf), 0);
                if (r <= 0) {
                    break;
                }
                total += r;
            }
            if (total > 0) {
                atomic_fetch_add(ctx->completed, 1);
            }
        }
        close(fd);
    }

    return NULL;
}

static void test_server_concurrent_load_and_shutdown(void) {
    enum { NUM_CLIENTS = 12, NUM_CYCLES = 3 };

    app_config_t cfg = {0};
    (void)snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    (void)snprintf(cfg.telegram_api_url, sizeof(cfg.telegram_api_url),
                   "http://127.0.0.1:1");
    cfg.allow_insecure_http = true;
    cfg.port = 0;
    cfg.max_body_bytes = 4096;
    cfg.worker_threads = 4;
    cfg.connect_timeout_ms = 500;
    cfg.request_timeout_ms = 1000;

    for (int cycle = 0; cycle < NUM_CYCLES; cycle++) {
        http_server_t *srv = http_server_create(&cfg);
        TEST_ASSERT(srv != NULL);
        uint16_t port = http_server_port(srv);
        TEST_ASSERT(port != 0);

        server_thread_ctx_t srv_ctx = {.srv = srv};
        pthread_t srv_thread;
        TEST_ASSERT(
            pthread_create(&srv_thread, NULL, server_thread_fn, &srv_ctx) == 0);

        atomic_int completed;
        atomic_init(&completed, 0);
        atomic_bool stop_flag;
        atomic_init(&stop_flag, false);

        pthread_t clients[NUM_CLIENTS];
        client_thread_ctx_t cctx[NUM_CLIENTS];
        for (int i = 0; i < NUM_CLIENTS; i++) {
            cctx[i].port = port;
            cctx[i].thread_id = i;
            cctx[i].completed = &completed;
            cctx[i].stop_flag = &stop_flag;
            TEST_ASSERT(pthread_create(&clients[i], NULL, client_thread_fn,
                                       &cctx[i]) == 0);
        }

        for (int wait_ms = 0; wait_ms < 25000 && atomic_load(&completed) < 40;
             wait_ms += 5) {
            usleep(5000);
        }
        TEST_ASSERT(atomic_load(&completed) >= 40);

        /* Trigger shutdown while client threads are still actively connecting
         */
        http_server_signal_stop(srv);
        atomic_store(&stop_flag, true);

        TEST_ASSERT(pthread_join(srv_thread, NULL) == 0);
        for (int i = 0; i < NUM_CLIENTS; i++) {
            TEST_ASSERT(pthread_join(clients[i], NULL) == 0);
        }
        http_server_destroy(srv);
    }
}

int main(void) {
    struct sigaction ign = {0};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, NULL);

    TEST_ASSERT(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);

    test_concurrent_pure_functions();
    test_server_create_failure_cleanup();
    test_server_concurrent_load_and_shutdown();

    curl_global_cleanup();
    printf("All test_concurrency checks passed!\n");
    return 0;
}
