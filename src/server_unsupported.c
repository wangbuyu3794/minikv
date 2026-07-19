#include "server_internal.h"

#include <errno.h>
#include <stdio.h>

static int unsupported_error(void)
{
#ifdef ENOTSUP
    return ENOTSUP;
#else
    return EINVAL;
#endif
}

int minikv_server_run(const struct minikv_server_options *options)
{
    if (options == NULL || options->bind_address == NULL) {
        errno = EINVAL;
        return 1;
    }

    fprintf(stderr,
            "minikv-server: network service is only supported on Linux\n");
    return 1;
}

int minikv_server_create(
    struct minikv_server **out_server,
    const struct minikv_server_options *options)
{
    if (out_server != NULL) {
        *out_server = NULL;
    }

    (void) options;
    errno = unsupported_error();
    return -1;
}

int minikv_server_poll(struct minikv_server *server, int timeout_ms)
{
    (void) server;
    (void) timeout_ms;
    errno = unsupported_error();
    return MINIKV_POLL_ERROR;
}

void minikv_server_destroy(struct minikv_server *server)
{
    (void) server;
}

uint16_t minikv_server_bound_port(const struct minikv_server *server)
{
    (void) server;
    return 0U;
}

int minikv_server_listener_fd(const struct minikv_server *server)
{
    (void) server;
    return -1;
}

int minikv_server_event_loop_fd(const struct minikv_server *server)
{
    (void) server;
    return -1;
}

size_t minikv_server_connection_count(const struct minikv_server *server)
{
    (void) server;
    return 0U;
}

int minikv_server_connection_fd(
    const struct minikv_server *server,
    size_t index)
{
    (void) server;
    (void) index;
    return -1;
}
