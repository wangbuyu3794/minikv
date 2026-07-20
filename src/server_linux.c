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

#include "command_internal.h"
#include "resp2_internal.h"

enum {
    MINIKV_MAX_EVENTS = 64,
    MINIKV_READ_BUFFER_SIZE = 4096
};

#define MINIKV_DEFAULT_OUTPUT_LIMIT ((size_t) 33554432U)

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
    struct minikv_server *server;
    struct minikv_resp2_parser *parser;
    unsigned char *output_buffer;
    size_t output_capacity;
    size_t output_offset;
    size_t output_length;
    uint32_t current_epoll_events;
    bool epoll_registered;
    bool closing;
    bool close_after_write;
    bool parser_has_incomplete_input;
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
    size_t output_limit;

#ifdef MINIKV_SERVER_TESTING
    size_t test_send_chunk_limit;
    bool test_force_next_send_would_block;
#endif

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

    minikv_resp2_parser_destroy(connection->parser);
    connection->parser = NULL;
    free(connection->output_buffer);
    connection->output_buffer = NULL;
    connection->output_capacity = 0U;
    connection->output_offset = 0U;
    connection->output_length = 0U;

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

static uint32_t connection_epoll_events(
    const struct minikv_connection *connection)
{
    uint32_t events = EPOLLERR | EPOLLHUP | EPOLLRDHUP;

    if (!connection->close_after_write) {
        events |= EPOLLIN;
    }

    if (connection->output_length > 0U) {
        events |= EPOLLOUT;
    }

    return events;
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
    connection->server = server;

    if (minikv_resp2_parser_create(&connection->parser, NULL) < 0) {
        saved_errno = ENOMEM;
        connection->event_source.fd = -1;

        if (close(accepted_fd) < 0) {
            report_error("close(unregistered client)", errno);
        }

        minikv_resp2_parser_destroy(connection->parser);
        free(connection);
        errno = saved_errno;
        return -1;
    }

    memset(&event, 0, sizeof(event));
    event.events = connection_epoll_events(connection);
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

        minikv_resp2_parser_destroy(connection->parser);
        free(connection);
        errno = saved_errno;
        return -1;
    }

    connection->epoll_registered = true;
    connection->current_epoll_events = event.events;
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

static int update_connection_interest(
    struct minikv_server *server,
    struct minikv_connection *connection)
{
    struct epoll_event event;
    uint32_t desired_events;

    if (connection->closing) {
        return -1;
    }

    if (connection->close_after_write &&
        connection->output_length == 0U) {
        close_connection(server, connection);
        return -1;
    }

    desired_events = connection_epoll_events(connection);

    if (desired_events == connection->current_epoll_events) {
        return 0;
    }

    memset(&event, 0, sizeof(event));
    event.events = desired_events;
    event.data.ptr = &connection->event_source;

    if (epoll_ctl(server->epoll_fd,
                  EPOLL_CTL_MOD,
                  connection->event_source.fd,
                  &event) < 0) {
        report_error("epoll_ctl(MOD client)", errno);
        close_connection(server, connection);
        return -1;
    }

    connection->current_epoll_events = desired_events;
    return 0;
}

static int ensure_output_capacity(
    struct minikv_connection *connection,
    size_t required)
{
    size_t new_capacity;
    unsigned char *new_buffer;

    if (required <= connection->output_capacity) {
        return 0;
    }

    new_capacity = connection->output_capacity;

    if (new_capacity == 0U) {
        new_capacity = required < 1024U ? 1024U : required;

        if (new_capacity > connection->server->output_limit) {
            new_capacity = connection->server->output_limit;
        }
    }

    while (new_capacity < required) {
        if (new_capacity > connection->server->output_limit / 2U) {
            new_capacity = connection->server->output_limit;
            break;
        }

        new_capacity *= 2U;
    }

    if (new_capacity < required ||
        new_capacity > connection->server->output_limit) {
        return -1;
    }

    new_buffer = realloc(connection->output_buffer, new_capacity);

    if (new_buffer == NULL) {
        return -1;
    }

    connection->output_buffer = new_buffer;
    connection->output_capacity = new_capacity;
    return 0;
}

static int append_encoded_reply(
    struct minikv_connection *connection,
    enum minikv_resp2_encode_type type,
    const unsigned char *payload,
    size_t payload_length)
{
    enum minikv_resp2_encode_error encode_error;
    size_t encoded_size = 0U;
    size_t required;
    size_t write_offset;
    size_t written = 0U;

    if (connection->output_offset > connection->output_capacity ||
        connection->output_length >
            connection->output_capacity - connection->output_offset) {
        report_internal_error("invalid output buffer state");
        return -1;
    }

    if (minikv_resp2_encoded_size(
            type,
            payload,
            payload_length,
            &encoded_size,
            &encode_error) < 0) {
        report_internal_error("response size calculation failed");
        return -1;
    }

    if (encoded_size > connection->server->output_limit ||
        connection->output_length >
            connection->server->output_limit - encoded_size) {
        return -1;
    }

    required = connection->output_length + encoded_size;
    write_offset =
        connection->output_offset + connection->output_length;

    if (encoded_size >
        connection->output_capacity - write_offset) {
        if (connection->output_offset > 0U) {
            memmove(
                connection->output_buffer,
                connection->output_buffer + connection->output_offset,
                connection->output_length);
            connection->output_offset = 0U;
            write_offset = connection->output_length;
        }

        if (ensure_output_capacity(connection, required) < 0) {
            return -1;
        }
    }

    if (minikv_resp2_encode(
            type,
            payload,
            payload_length,
            connection->output_buffer + write_offset,
            connection->output_capacity - write_offset,
            &written,
            &encode_error) < 0 ||
        written != encoded_size) {
        report_internal_error("response encoding failed");
        return -1;
    }

    connection->output_length += written;
    return 0;
}

static int parser_error_is_client_error(
    enum minikv_resp2_error error)
{
    switch (error) {
    case MINIKV_RESP2_ERROR_UNKNOWN_TYPE:
    case MINIKV_RESP2_ERROR_INVALID_CRLF:
    case MINIKV_RESP2_ERROR_INVALID_INTEGER:
    case MINIKV_RESP2_ERROR_INTEGER_OVERFLOW:
    case MINIKV_RESP2_ERROR_INVALID_LENGTH:
    case MINIKV_RESP2_ERROR_LENGTH_OVERFLOW:
    case MINIKV_RESP2_ERROR_LIMIT_EXCEEDED:
    case MINIKV_RESP2_ERROR_NESTING_TOO_DEEP:
        return 1;

    case MINIKV_RESP2_ERROR_NONE:
    case MINIKV_RESP2_ERROR_INVALID_ARGUMENT:
    case MINIKV_RESP2_ERROR_OUT_OF_MEMORY:
    case MINIKV_RESP2_ERROR_INVALID_STATE:
        return 0;
    }

    return 0;
}

static int queue_protocol_error(
    struct minikv_server *server,
    struct minikv_connection *connection)
{
    static const unsigned char payload[] = "ERR Protocol error";

    if (append_encoded_reply(
            connection,
            MINIKV_RESP2_ENCODE_SIMPLE_ERROR,
            payload,
            sizeof(payload) - 1U) < 0) {
        close_connection(server, connection);
        return -1;
    }

    connection->close_after_write = true;

    if (update_connection_interest(server, connection) < 0) {
        return -1;
    }

    return 0;
}

static int process_received_chunk(
    struct minikv_server *server,
    struct minikv_connection *connection,
    const unsigned char *buffer,
    size_t length)
{
    size_t offset = 0U;

    while (offset < length) {
        struct minikv_resp2_value *value = NULL;
        enum minikv_resp2_parse_result parse_result;
        size_t consumed = 0U;
        size_t remaining = length - offset;

        parse_result = minikv_resp2_parser_feed(
            connection->parser,
            buffer + offset,
            remaining,
            &consumed,
            &value);

        switch (parse_result) {
        case MINIKV_RESP2_NEED_MORE:
            if (value != NULL || consumed != remaining) {
                minikv_resp2_value_destroy(value);
                report_internal_error("invalid parser NEED_MORE result");
                close_connection(server, connection);
                return -1;
            }

            if (consumed > 0U) {
                connection->parser_has_incomplete_input = true;
            }

            return 0;

        case MINIKV_RESP2_VALUE_READY:
            if (value == NULL || consumed == 0U ||
                consumed > remaining) {
                minikv_resp2_value_destroy(value);
                report_internal_error("invalid parser VALUE_READY result");
                close_connection(server, connection);
                return -1;
            }

            offset += consumed;
            connection->parser_has_incomplete_input = false;

            {
                struct minikv_command_reply reply;
                enum minikv_command_dispatch_result dispatch_result;

                dispatch_result =
                    minikv_command_dispatch(value, &reply);

                switch (dispatch_result) {
                case MINIKV_COMMAND_DISPATCH_REPLY:
                    if (append_encoded_reply(
                            connection,
                            reply.type,
                            reply.payload,
                            reply.payload_length) < 0) {
                        minikv_resp2_value_destroy(value);
                        close_connection(server, connection);
                        return -1;
                    }

                    minikv_resp2_value_destroy(value);
                    break;

                case MINIKV_COMMAND_DISPATCH_PROTOCOL_ERROR:
                    minikv_resp2_value_destroy(value);
                    (void) queue_protocol_error(server, connection);
                    return -1;

                case MINIKV_COMMAND_DISPATCH_INTERNAL_ERROR:
                    minikv_resp2_value_destroy(value);
                    close_connection(server, connection);
                    return -1;

                default:
                    minikv_resp2_value_destroy(value);
                    report_internal_error(
                        "unknown command dispatch result");
                    close_connection(server, connection);
                    return -1;
                }
            }

            break;

        case MINIKV_RESP2_PARSE_ERROR:
            minikv_resp2_value_destroy(value);

            {
                enum minikv_resp2_error parser_error =
                    minikv_resp2_parser_error(connection->parser);

                if (parser_error_is_client_error(parser_error)) {
                    (void) queue_protocol_error(server, connection);
                } else {
                    report_internal_error("internal parser failure");
                    close_connection(server, connection);
                }
            }

            return -1;

        default:
            minikv_resp2_value_destroy(value);
            report_internal_error("unknown parser result");
            close_connection(server, connection);
            return -1;
        }
    }

    return 0;
}

static void handle_peer_shutdown(
    struct minikv_server *server,
    struct minikv_connection *connection)
{
    if (connection->closing) {
        return;
    }

    if (connection->close_after_write) {
        if (connection->output_length == 0U) {
            close_connection(server, connection);
        } else {
            (void) update_connection_interest(server, connection);
        }

        return;
    }

    if (connection->parser_has_incomplete_input) {
        close_connection(server, connection);
        return;
    }

    if (connection->output_length == 0U) {
        close_connection(server, connection);
        return;
    }

    connection->close_after_write = true;
    (void) update_connection_interest(server, connection);
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
            if (process_received_chunk(
                    server,
                    connection,
                    buffer,
                    (size_t) received) < 0) {
                return connection->closing
                    ? MINIKV_RECEIVE_CLOSED
                    : MINIKV_RECEIVE_OPEN;
            }

            if (update_connection_interest(server, connection) < 0) {
                return MINIKV_RECEIVE_FAILED;
            }

            if (connection->close_after_write) {
                return MINIKV_RECEIVE_OPEN;
            }

            continue;
        }

        if (received == 0) {
            handle_peer_shutdown(server, connection);
            return connection->closing
                ? MINIKV_RECEIVE_CLOSED
                : MINIKV_RECEIVE_OPEN;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (update_connection_interest(server, connection) < 0) {
                return MINIKV_RECEIVE_FAILED;
            }

            return MINIKV_RECEIVE_OPEN;
        }

        report_error("recv", errno);
        close_connection(server, connection);
        return MINIKV_RECEIVE_FAILED;
    }
}

static int drain_connection_output(
    struct minikv_server *server,
    struct minikv_connection *connection)
{
    while (connection->output_length > 0U) {
        size_t send_length = connection->output_length;
        ssize_t sent;

#ifdef MINIKV_SERVER_TESTING
        if (server->test_send_chunk_limit > 0U &&
            send_length > server->test_send_chunk_limit) {
            send_length = server->test_send_chunk_limit;
        }

        if (server->test_force_next_send_would_block) {
            server->test_force_next_send_would_block = false;
            return 0;
        }
#endif

        sent = send(
            connection->event_source.fd,
            connection->output_buffer + connection->output_offset,
            send_length,
            MSG_NOSIGNAL);

        if (sent > 0) {
            size_t sent_size = (size_t) sent;

            if (sent_size > connection->output_length) {
                report_internal_error("send exceeded pending output");
                close_connection(server, connection);
                return -1;
            }

            connection->output_offset += sent_size;
            connection->output_length -= sent_size;
            continue;
        }

        if (sent == 0) {
            report_internal_error("send made no progress");
            close_connection(server, connection);
            return -1;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }

        report_error("send", errno);
        close_connection(server, connection);
        return -1;
    }

    connection->output_offset = 0U;

    if (connection->close_after_write) {
        close_connection(server, connection);
        return -1;
    }

    return update_connection_interest(server, connection);
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
        } else if (socket_error != 0) {
            report_error("client socket", socket_error);
        } else {
            report_internal_error("client EPOLLERR without SO_ERROR");
        }

        close_connection(server, connection);
        return;
    }

    if ((events & EPOLLIN) != 0U &&
        !connection->close_after_write) {
        (void) drain_connection_input(server, connection);
    }

    if (connection->closing) {
        return;
    }

    if ((events & EPOLLOUT) != 0U &&
        connection->output_length > 0U) {
        (void) drain_connection_output(server, connection);
    }

    if (connection->closing) {
        return;
    }

    if ((events & (EPOLLHUP | EPOLLRDHUP)) != 0U) {
        handle_peer_shutdown(server, connection);
    }

    if (!connection->closing) {
        (void) update_connection_interest(server, connection);
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
    server->output_limit = MINIKV_DEFAULT_OUTPUT_LIMIT;

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

#ifdef MINIKV_SERVER_TESTING

int minikv_server_test_set_send_chunk_limit(
    struct minikv_server *server,
    size_t limit)
{
    if (server == NULL) {
        return -1;
    }

    server->test_send_chunk_limit = limit;
    return 0;
}

int minikv_server_test_force_next_send_would_block(
    struct minikv_server *server)
{
    if (server == NULL) {
        return -1;
    }

    server->test_force_next_send_would_block = true;
    return 0;
}

int minikv_server_test_set_output_limit(
    struct minikv_server *server,
    size_t limit)
{
    if (server == NULL || limit == 0U) {
        return -1;
    }

    server->output_limit = limit;
    return 0;
}

#endif

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
