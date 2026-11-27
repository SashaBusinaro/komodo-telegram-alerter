#ifndef KOMODO_ALERTER_HTTP_SERVER_H
#define KOMODO_ALERTER_HTTP_SERVER_H

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>

typedef struct http_server http_server_t;

/**
 * Format a socket address (IPv4 or IPv6) as "ip:port" or "[ip]:port".
 * Unwraps IPv4-mapped IPv6 addresses (::ffff:a.b.c.d) to "a.b.c.d:port".
 * Writes "unknown" if addr is NULL, truncated, or uses an unsupported family.
 */
void http_format_peer_addr(const struct sockaddr *addr, socklen_t addr_len,
                           char *out, size_t out_size);

/**
 * Extract and percent-decode a query parameter by exact key name.
 *
 * Returns true if the query syntax/encoding is valid (with *found indicating
 * whether the key was present). Returns false if percent-encoding is malformed,
 * contains an embedded NUL (%00), or the decoded value exceeds out_size.
 */
bool http_query_get_param(const char *query, size_t query_len, const char *key,
                          char *out, size_t out_size, bool *found);

/**
 * Create, bind, and start the worker pool for the HTTP/1.1 server.
 * Returns NULL on failure.
 */
http_server_t *http_server_create(const app_config_t *cfg);

/**
 * Retrieve the bound port (useful when cfg->port is non-zero or ephemeral).
 */
uint16_t http_server_port(const http_server_t *srv);

/**
 * Run the accept loop until http_server_signal_stop() is invoked.
 */
void http_server_run(http_server_t *srv);

/**
 * Async-signal-safe shutdown trigger (safe to call from SIGINT/SIGTERM
 * handlers).
 */
void http_server_signal_stop(http_server_t *srv);

/**
 * Join all worker threads and release server resources.
 */
void http_server_destroy(http_server_t *srv);

#endif /* KOMODO_ALERTER_HTTP_SERVER_H */
