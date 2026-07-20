#ifndef MINIKV_SERVER_INTERNAL_H_INCLUDED
#define MINIKV_SERVER_INTERNAL_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

struct minikv_server;

struct minikv_server_options {
    /*
     * Borrowed, NUL-terminated numeric IPv4 string.
     * The caller retains ownership for the duration of the call.
     */
    const char *bind_address;

    /*
     * Host-order TCP port. Zero requests an ephemeral port.
     */
    uint16_t port;
};

enum minikv_server_poll_result {
    MINIKV_POLL_ERROR = -1,
    MINIKV_POLL_CONTINUE = 0,
    MINIKV_POLL_STOP = 1
};

/*
 * Runs the server until a normal stop request or fatal error.
 *
 * Does not take ownership of options or options->bind_address.
 * Returns zero after a normal stop and nonzero after an error.
 */
int minikv_server_run(const struct minikv_server_options *options);

/*
 * Internal lifecycle interface used by the runtime and Linux network tests.
 *
 * On success, *out_server is owned by the caller and must be passed to
 * minikv_server_destroy(). On failure, *out_server remains NULL.
 */
int minikv_server_create(
    struct minikv_server **out_server,
    const struct minikv_server_options *options);

int minikv_server_poll(struct minikv_server *server, int timeout_ms);

void minikv_server_destroy(struct minikv_server *server);

/*
 * Internal test observability. These functions do not transfer ownership.
 * Every returned fd is borrowed and is valid only while server owns it.
 */
uint16_t minikv_server_bound_port(const struct minikv_server *server);
int minikv_server_listener_fd(const struct minikv_server *server);

/*
 * Returns the borrowed internal event-loop fd for internal test observation.
 * The caller must not close it. The value becomes invalid after server is
 * destroyed.
 */
int minikv_server_event_loop_fd(const struct minikv_server *server);

size_t minikv_server_connection_count(const struct minikv_server *server);
int minikv_server_connection_fd(
    const struct minikv_server *server,
    size_t index);

#ifdef MINIKV_SERVER_TESTING

int minikv_server_test_set_send_chunk_limit(
    struct minikv_server *server,
    size_t limit);

int minikv_server_test_force_next_send_would_block(
    struct minikv_server *server);

int minikv_server_test_set_output_limit(
    struct minikv_server *server,
    size_t limit);

#endif

#endif
