#include "config.h"
#include "http_server.h"
#include "logging.h"

#include <curl/curl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>

static _Atomic(http_server_t *) g_server = NULL;

static void handle_shutdown_signal(int sig) {
    (void)sig;
    http_server_t *srv = atomic_load_explicit(&g_server, memory_order_acquire);
    if (srv != NULL) {
        http_server_signal_stop(srv);
    }
}

int main(void) {
    log_init();

    struct sigaction ign = {0};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, NULL);

    app_config_t cfg;
    if (!config_load_from_env(&cfg)) {
        return EXIT_FAILURE;
    }
    log_set_level(cfg.log_level);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        log_error("Failed to initialize libcurl");
        return EXIT_FAILURE;
    }

    http_server_t *srv = http_server_create(&cfg);
    if (srv == NULL) {
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    atomic_store_explicit(&g_server, srv, memory_order_release);

    struct sigaction sa = {0};
    sa.sa_handler = handle_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    http_server_run(srv);

    struct sigaction dfl = {0};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(SIGINT, &dfl, NULL);
    sigaction(SIGTERM, &dfl, NULL);

    log_info("Shutting down gracefully");
    atomic_store_explicit(&g_server, NULL, memory_order_release);
    http_server_destroy(srv);
    curl_global_cleanup();
    return EXIT_SUCCESS;
}
