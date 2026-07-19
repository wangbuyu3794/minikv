#include "server_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    MINIKV_MAX_EVENTS = 64,
    MINIKV_READ_BUFFER_SIZE = 4096
};

enum minikv_event_source_kind {
    MINIKV_EVENT_LISTENER,
    MINIKV_EVENT_SIGNAL,
    MINIKV_EVENT_CONNECTION
};

struct minikv_event_source {
    enum minikv_event_source_kind kind;
    int fd;
    void *owner;
};

struct minikv_connection {
    struct minikv_event_source event_source;
    bool epoll_registered;
    bool closing;
    struct minikv_connection *next;
};

struct minikv_server {
    int epoll_fd;

    struct minikv_event_source listener_source;
    bool listener_registered;

    struct minikv_event_source signal_source;
    bool signal_registered;

    struct minikv_connection *active_connections;
    struct minikv_connection *retired_connections;
    size_t connection_count;

    uint16_t bound_port;
};

enum minikv_receive_result {
    MINIKV_RECEIVE_OPEN,
    MINIKV_RECEIVE_CLOSED,
    MINIKV_RECEIVE_FAILED
};

static void report_error(const char *operation, int error_number)
{
    fprintf(stderr,
            "minikv-server: %s: %s\n",
            operation,
            strerror(error_number));
}

static void report_internal_error(const char *message)
{
    fprintf(stderr, "minikv-server: internal error: %s\n", message);
}

static int close_owned_fd(int *owned_fd, const char *operation)
{
    int fd;

    if (owned_fd == NULL || *owned_fd < 0) {
        return 0;
    }

    fd = *owned_fd;
    *owned_fd = -1;

    if (close(fd) < 0) {
        report_error(operation, errno);
        return -1;
    }

    return 0;
}

static int unregister_and_close_source(
    struct minikv_server *server,
    struct minikv_event_source *source,
    bool *registered,
    const char *delete_operation,
    const char *close_operation)
{
    int result = 0;

    if (source == NULL || registered == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (*registered && source->fd >= 0 &&
        server != NULL && server->epoll_fd >= 0) {
        if (epoll_ctl(server->epoll_fd,
                      EPOLL_CTL_DEL,
                      source->fd,
                      NULL) < 0) {
            report_error(delete_operation, errno);
            result = -1;
        }
    }

    *registered = false;

    if (close_owned_fd(&source->fd, close_operation) < 0) {
        result = -1;
    }

    return result;
}

static void close_connection(
    struct minikv_server *server,
    struct minikv_connection *connection)
{
    struct minikv_connection **link;
    int fd;

    if (server == NULL || connection == NULL || connection->closing) {
        return;
    }

    connection->closing = true;

    link = &server->active_connections;

    while (*link != NULL && *link != connection) {
        link = &(*link)->next;
    }

    if (*link == connection) {
        *link = connection->next;

        if (server->connection_count > 0U) {
            server->connection_count--;
        } else {
            report_internal_error("connection count underflow");
        }
    } else {
        report_internal_error("active connection not found");
    }

    connection->next = NULL;
    fd = connection->event_source.fd;
    connection->event_source.fd = -1;

    if (fd >= 0 && connection->epoll_registered) {
        if (epoll_ctl(server->epoll_fd,
                      EPOLL_CTL_DEL,
                      fd,
                      NULL) < 0) {
            report_error("epoll_ctl(DEL client)", errno);
        }

        connection->epoll_registered = false;
    }

    if (fd >= 0 && close(fd) < 0) {
        report_error("close(client)", errno);
    }

    connection->next = server->retired_connections;
    server->retired_connections = connection;
}

static void free_retired_connections(struct minikv_server *server)
{
    struct minikv_connection *connection;

    connection = server->retired_connections;
    server->retired_connections = NULL;

    while (connection != NULL) {
        struct minikv_connection *next = connection->next;
        free(connection);
        connection = next;
    }
}

static int register_connection(
    struct minikv_server *server,
    int accepted_fd)
{
    struct minikv_connection *connection;
    struct epoll_event event;
    int saved_errno;

    if (server->connection_count == SIZE_MAX) {
        report_internal_error("connection count overflow");

        if (close(accepted_fd) < 0) {
            report_error("close(untracked client)", errno);
        }

        errno = EOVERFLOW;
        return -1;
    }

    connection = calloc(1U, sizeof(*connection));

    if (connection == NULL) {
        saved_errno = errno;

        if (close(accepted_fd) < 0) {
            report_error("close(untracked client)", errno);
        }

        errno = saved_errno;
        return -1;
    }

    connection->event_source.kind = MINIKV_EVENT_CONNECTION;
    connection->event_source.fd = accepted_fd;
    connection->event_source.owner = connection;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    event.data.ptr = &connection->event_source;

    if (epoll_ctl(server->epoll_fd,
                  EPOLL_CTL_ADD,
                  accepted_fd,
                  &event) < 0) {
        saved_errno = errno;
        connection->event_source.fd = -1;

        if (close(accepted_fd) < 0) {
            report_error("close(unregistered client)", errno);
        }

        free(connection);
        errno = saved_errno;
        return -1;
    }

    connection->epoll_registered = true;
    connection->next = server->active_connections;
    server->active_connections = connection;
    server->connection_count++;
    return 0;
}

static int accept_ready_connections(struct minikv_server *server)
{
    for (;;) {
        int accepted_fd;

        accepted_fd = accept4(server->listener_source.fd,
                              NULL,
                              NULL,
                              SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (accepted_fd >= 0) {
            if (register_connection(server, accepted_fd) < 0) {
                report_error("register accepted client", errno);
                return -1;
            }

            continue;
        }

        if (errno == EINTR || errno == ECONNABORTED) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }

        report_error("accept4", errno);
        return -1;
    }
}

static enum minikv_receive_result drain_connection_input(
    struct minikv_server *server,
    struct minikv_connection *connection)
{
    unsigned char buffer[MINIKV_READ_BUFFER_SIZE];

    for (;;) {
        ssize_t received;

        received = recv(connection->event_source.fd,
                        buffer,
                        sizeof(buffer),
                        0);

        if (received > 0) {
            continue;
        }

        if (received == 0) {
            close_connection(server, connection);
            return MINIKV_RECEIVE_CLOSED;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MINIKV_RECEIVE_OPEN;
        }

        report_error("recv", errno);
        close_connection(server, connection);
        return MINIKV_RECEIVE_FAILED;
    }
}

static void handle_connection_event(
    struct minikv_server *server,
    struct minikv_connection *connection,
    uint32_t events)
{
    if (connection->closing) {
        return;
    }

    if ((events & EPOLLERR) != 0U) {
        int socket_error = 0;
        socklen_t error_length = (socklen_t) sizeof(socket_error);

        if (getsockopt(connection->event_source.fd,
                       SOL_SOCKET,
                       SO_ERROR,
                       &socket_error,
                       &error_length) < 0) {
            report_error("getsockopt(client SO_ERROR)", errno);
            close_connection(server, connection);
            return;
        }

        if (socket_error != 0) {
            report_error("client socket", socket_error);
            close_connection(server, connection);
            return;
        }
    }

    if ((events & EPOLLIN) != 0U) {
        enum minikv_receive_result receive_result;

        receive_result = drain_connection_input(server, connection);

        if (receive_result != MINIKV_RECEIVE_OPEN) {
            return;
        }
    }

    if (connection->closing) {
        return;
    }

    if ((events & (EPOLLHUP | EPOLLRDHUP)) != 0U) {
        close_connection(server, connection);
    }
}

static int handle_listener_event(
    struct minikv_server *server,
    uint32_t events)
{
    if ((events & EPOLLERR) != 0U) {
        int socket_error = 0;
        socklen_t error_length = (socklen_t) sizeof(socket_error);

        if (getsockopt(server->listener_source.fd,
                       SOL_SOCKET,
                       SO_ERROR,
                       &socket_error,
                       &error_length) < 0) {
            report_error("getsockopt(listener SO_ERROR)", errno);
            return MINIKV_POLL_ERROR;
        }

        if (socket_error != 0) {
            report_error("listener socket", socket_error);
            return MINIKV_POLL_ERROR;
        }
    }

    if ((events & EPOLLHUP) != 0U) {
        report_internal_error("listener hangup");
        return MINIKV_POLL_ERROR;
    }

    if ((events & EPOLLIN) != 0U &&
        accept_ready_connections(server) < 0) {
        return MINIKV_POLL_ERROR;
    }

    return MINIKV_POLL_CONTINUE;
}

static int drain_signal_fd(struct minikv_server *server)
{
    bool stop_requested = false;

    for (;;) {
        struct signalfd_siginfo info;
        ssize_t bytes_read;

        bytes_read = read(server->signal_source.fd,
                          &info,
                          sizeof(info));

        if (bytes_read == (ssize_t) sizeof(info)) {
            if (info.ssi_signo == (uint32_t) SIGINT ||
                info.ssi_signo == (uint32_t) SIGTERM) {
                stop_requested = true;
            }

            continue;
        }

        if (bytes_read < 0 && errno == EINTR) {
            continue;
        }

        if (bytes_read < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }

        if (bytes_read == 0) {
            report_internal_error("unexpected signalfd EOF");
        } else if (bytes_read >= 0) {
            report_internal_error("short signalfd read");
        } else {
            report_error("read(signalfd)", errno);
        }

        return MINIKV_POLL_ERROR;
    }

    return stop_requested ? MINIKV_POLL_STOP : MINIKV_POLL_CONTINUE;
}

static int handle_signal_event(
    struct minikv_server *server,
    uint32_t events)
{
    int result = MINIKV_POLL_CONTINUE;

    if ((events & EPOLLIN) != 0U) {
        result = drain_signal_fd(server);

        if (result != MINIKV_POLL_CONTINUE) {
            return result;
        }
    }

    if ((events & (EPOLLERR | EPOLLHUP)) != 0U) {
        report_internal_error("signalfd error or hangup");
        return MINIKV_POLL_ERROR;
    }

    return result;
}

int minikv_server_create(
    struct minikv_server **out_server,
    const struct minikv_server_options *options)
{
    struct minikv_server *server;
    struct sockaddr_in address;
    struct epoll_event event;
    socklen_t address_length;
    int reuse_address = 1;
    int saved_errno;

    if (out_server == NULL || options == NULL ||
        options->bind_address == NULL) {
        errno = EINVAL;
        return -1;
    }

    *out_server = NULL;
    server = calloc(1U, sizeof(*server));

    if (server == NULL) {
        return -1;
    }

    server->epoll_fd = -1;
    server->listener_source.kind = MINIKV_EVENT_LISTENER;
    server->listener_source.fd = -1;
    server->listener_source.owner = server;
    server->signal_source.kind = MINIKV_EVENT_SIGNAL;
    server->signal_source.fd = -1;
    server->signal_source.owner = server;

    server->listener_source.fd =
        socket(AF_INET,
               SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
               0);

    if (server->listener_source.fd < 0) {
        goto fail;
    }

    if (setsockopt(server->listener_source.fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &reuse_address,
                   (socklen_t) sizeof(reuse_address)) < 0) {
        goto fail;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(options->port);

    {
        int address_result;

        address_result = inet_pton(AF_INET,
                                   options->bind_address,
                                   &address.sin_addr);

        if (address_result < 0) {
            goto fail;
        }

        if (address_result == 0) {
            errno = EINVAL;
            goto fail;
        }
    }

    if (bind(server->listener_source.fd,
             (const struct sockaddr *) &address,
             (socklen_t) sizeof(address)) < 0) {
        goto fail;
    }

    if (listen(server->listener_source.fd, SOMAXCONN) < 0) {
        goto fail;
    }

    address_length = (socklen_t) sizeof(address);

    if (getsockname(server->listener_source.fd,
                    (struct sockaddr *) &address,
                    &address_length) < 0) {
        goto fail;
    }

    server->bound_port = ntohs(address.sin_port);
    server->epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    if (server->epoll_fd < 0) {
        goto fail;
    }

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    event.data.ptr = &server->listener_source;

    if (epoll_ctl(server->epoll_fd,
                  EPOLL_CTL_ADD,
                  server->listener_source.fd,
                  &event) < 0) {
        goto fail;
    }

    server->listener_registered = true;
    *out_server = server;
    return 0;

fail:
    saved_errno = errno;
    minikv_server_destroy(server);
    errno = saved_errno;
    return -1;
}

int minikv_server_poll(struct minikv_server *server, int timeout_ms)
{
    struct epoll_event events[MINIKV_MAX_EVENTS];
    int event_count;
    int result = MINIKV_POLL_ERROR;
    int index;

    if (server == NULL) {
        errno = EINVAL;
        return MINIKV_POLL_ERROR;
    }

    if (server->epoll_fd < 0 || timeout_ms < -1) {
        errno = EINVAL;
        goto done;
    }

    for (;;) {
        event_count = epoll_wait(server->epoll_fd,
                                 events,
                                 MINIKV_MAX_EVENTS,
                                 timeout_ms);

        if (event_count >= 0) {
            break;
        }

        if (errno != EINTR) {
            report_error("epoll_wait", errno);
            goto done;
        }
    }

    result = MINIKV_POLL_CONTINUE;

    for (index = 0; index < event_count; index++) {
        struct minikv_event_source *source;

        source = events[index].data.ptr;

        if (source == NULL || source->owner == NULL) {
            report_internal_error("epoll event without owner");
            result = MINIKV_POLL_ERROR;
            break;
        }

        switch (source->kind) {
        case MINIKV_EVENT_LISTENER:
            result = handle_listener_event(
                source->owner,
                events[index].events);
            break;

        case MINIKV_EVENT_SIGNAL:
            result = handle_signal_event(
                source->owner,
                events[index].events);
            break;

        case MINIKV_EVENT_CONNECTION:
            handle_connection_event(
                server,
                source->owner,
                events[index].events);
            result = MINIKV_POLL_CONTINUE;
            break;

        default:
            report_internal_error("unknown epoll event source");
            result = MINIKV_POLL_ERROR;
            break;
        }

        if (result != MINIKV_POLL_CONTINUE) {
            break;
        }
    }

done:
    free_retired_connections(server);
    return result;
}

void minikv_server_destroy(struct minikv_server *server)
{
    if (server == NULL) {
        return;
    }

    while (server->active_connections != NULL) {
        close_connection(server, server->active_connections);
    }

    (void) unregister_and_close_source(
        server,
        &server->signal_source,
        &server->signal_registered,
        "epoll_ctl(DEL signalfd)",
        "close(signalfd)");

    (void) unregister_and_close_source(
        server,
        &server->listener_source,
        &server->listener_registered,
        "epoll_ctl(DEL listener)",
        "close(listener)");

    (void) close_owned_fd(&server->epoll_fd, "close(epoll)");
    free_retired_connections(server);
    free(server);
}

uint16_t minikv_server_bound_port(const struct minikv_server *server)
{
    return server == NULL ? 0U : server->bound_port;
}

int minikv_server_listener_fd(const struct minikv_server *server)
{
    return server == NULL ? -1 : server->listener_source.fd;
}

int minikv_server_event_loop_fd(const struct minikv_server *server)
{
    return server == NULL ? -1 : server->epoll_fd;
}

size_t minikv_server_connection_count(const struct minikv_server *server)
{
    return server == NULL ? 0U : server->connection_count;
}

int minikv_server_connection_fd(
    const struct minikv_server *server,
    size_t index)
{
    const struct minikv_connection *connection;

    if (server == NULL) {
        return -1;
    }

    connection = server->active_connections;

    while (connection != NULL && index > 0U) {
        connection = connection->next;
        index--;
    }

    return connection == NULL ? -1 : connection->event_source.fd;
}

int minikv_server_run(const struct minikv_server_options *options)
{
    struct minikv_server *server = NULL;
    sigset_t signal_mask;
    sigset_t old_mask;
    bool mask_saved = false;
    int result = 1;

    if (minikv_server_create(&server, options) < 0) {
        report_error("server initialization", errno);
        goto done;
    }

    if (sigemptyset(&signal_mask) < 0 ||
        sigaddset(&signal_mask, SIGINT) < 0 ||
        sigaddset(&signal_mask, SIGTERM) < 0) {
        report_error("build signal mask", errno);
        goto done;
    }

    if (sigprocmask(SIG_BLOCK, &signal_mask, &old_mask) < 0) {
        report_error("sigprocmask(SIG_BLOCK)", errno);
        goto done;
    }

    mask_saved = true;
    server->signal_source.fd =
        signalfd(-1,
                 &signal_mask,
                 SFD_NONBLOCK | SFD_CLOEXEC);

    if (server->signal_source.fd < 0) {
        report_error("signalfd", errno);
        goto done;
    }

    {
        struct epoll_event event;

        memset(&event, 0, sizeof(event));
        event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
        event.data.ptr = &server->signal_source;

        if (epoll_ctl(server->epoll_fd,
                      EPOLL_CTL_ADD,
                      server->signal_source.fd,
                      &event) < 0) {
            report_error("epoll_ctl(ADD signalfd)", errno);
            goto done;
        }

        server->signal_registered = true;
    }

    if (printf("MiniKV listening on %s:%u\n",
               options->bind_address,
               (unsigned int) server->bound_port) < 0) {
        report_error("write startup message",
                     errno == 0 ? EIO : errno);
        goto done;
    }

    if (fflush(stdout) == EOF) {
        report_error("flush startup message",
                     errno == 0 ? EIO : errno);
        goto done;
    }

    for (;;) {
        int poll_result = minikv_server_poll(server, -1);

        if (poll_result == MINIKV_POLL_STOP) {
            result = 0;
            break;
        }

        if (poll_result == MINIKV_POLL_ERROR) {
            break;
        }
    }

done:
    if (server != NULL) {
        if (unregister_and_close_source(
                server,
                &server->signal_source,
                &server->signal_registered,
                "epoll_ctl(DEL signalfd)",
                "close(signalfd)") < 0) {
            result = 1;
        }
    }

    minikv_server_destroy(server);

    if (mask_saved &&
        sigprocmask(SIG_SETMASK, &old_mask, NULL) < 0) {
        report_error("sigprocmask(SIG_SETMASK)", errno);
        result = 1;
    }

    return result;
}
