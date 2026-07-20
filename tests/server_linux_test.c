#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "server_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CLIENT_COUNT 3U
#define EVENT_TIMEOUT_MS 1000
#define IO_TIMEOUT_MS 2000
#define CLEANUP_TIMEOUT_MS 3000

struct test_case {
    const char *name;
    int (*run)(void);
};

static int make_deadline(struct timespec *deadline, int timeout_ms)
{
    if (deadline == NULL || timeout_ms < 0) {
        errno = EINVAL;
        return -1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, deadline) < 0) {
        return -1;
    }

    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long) (timeout_ms % 1000) * 1000000L;

    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }

    return 0;
}

static int remaining_timeout_ms(const struct timespec *deadline)
{
    struct timespec now;
    int64_t seconds;
    int64_t nanoseconds;
    int64_t milliseconds;

    if (deadline == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }

    seconds = (int64_t) deadline->tv_sec - (int64_t) now.tv_sec;
    nanoseconds = (int64_t) deadline->tv_nsec - (int64_t) now.tv_nsec;

    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000LL;
    }

    if (seconds < 0 || (seconds == 0 && nanoseconds == 0)) {
        return 0;
    }

    milliseconds = seconds * 1000LL +
                   (nanoseconds + 999999LL) / 1000000LL;

    if (milliseconds > INT_MAX) {
        return INT_MAX;
    }

    return (int) milliseconds;
}

static int close_owned_fd(int *fd)
{
    int owned_fd;

    if (fd == NULL || *fd < 0) {
        return 0;
    }

    owned_fd = *fd;
    *fd = -1;

    if (close(owned_fd) < 0) {
        fprintf(stderr, "close(%d) failed: %s\n", owned_fd, strerror(errno));
        return -1;
    }

    return 0;
}

static int check_fd_flags(
    const char *label,
    int fd,
    bool require_nonblocking)
{
    int status_flags;
    int descriptor_flags;

    status_flags = fcntl(fd, F_GETFL);

    if (status_flags < 0) {
        fprintf(stderr, "%s F_GETFL failed: %s\n", label, strerror(errno));
        return -1;
    }

    if (require_nonblocking && (status_flags & O_NONBLOCK) == 0) {
        fprintf(stderr, "%s is not nonblocking\n", label);
        return -1;
    }

    descriptor_flags = fcntl(fd, F_GETFD);

    if (descriptor_flags < 0) {
        fprintf(stderr, "%s F_GETFD failed: %s\n", label, strerror(errno));
        return -1;
    }

    if ((descriptor_flags & FD_CLOEXEC) == 0) {
        fprintf(stderr, "%s is not close-on-exec\n", label);
        return -1;
    }

    return 0;
}

static int expect_closed_fd(const char *label, int fd)
{
    int result;

    errno = 0;
    result = fcntl(fd, F_GETFD);

    if (result != -1 || errno != EBADF) {
        fprintf(stderr,
                "%s remained valid after destroy: result=%d errno=%d\n",
                label,
                result,
                errno);
        return -1;
    }

    return 0;
}

static int connect_client(uint16_t port)
{
    struct sockaddr_in address;
    struct timespec deadline;
    int client_fd = -1;
    int status_flags;
    int descriptor_flags;
    int socket_error;
    int connect_result;

    client_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (client_fd < 0) {
        fprintf(stderr, "socket(client) failed: %s\n", strerror(errno));
        return -1;
    }

    status_flags = fcntl(client_fd, F_GETFL);
    descriptor_flags = fcntl(client_fd, F_GETFD);

    if (status_flags < 0 || descriptor_flags < 0 ||
        fcntl(client_fd, F_SETFL, status_flags | O_NONBLOCK) < 0 ||
        fcntl(client_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        fprintf(stderr, "fcntl(client) failed: %s\n", strerror(errno));
        (void) close_owned_fd(&client_fd);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        fprintf(stderr, "inet_pton(client) failed\n");
        (void) close_owned_fd(&client_fd);
        return -1;
    }

    connect_result = connect(
        client_fd,
        (const struct sockaddr *) &address,
        sizeof(address));

    if (connect_result == 0) {
        return client_fd;
    }

    if (errno != EINPROGRESS && errno != EINTR) {
        fprintf(stderr, "connect(client) failed: %s\n", strerror(errno));
        (void) close_owned_fd(&client_fd);
        return -1;
    }

    if (make_deadline(&deadline, IO_TIMEOUT_MS) < 0) {
        fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
        (void) close_owned_fd(&client_fd);
        return -1;
    }

    for (;;) {
        struct pollfd descriptor;
        socklen_t error_length = sizeof(socket_error);
        int remaining = remaining_timeout_ms(&deadline);
        int poll_result;

        if (remaining < 0) {
            fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
            (void) close_owned_fd(&client_fd);
            return -1;
        }

        if (remaining == 0) {
            fprintf(stderr, "timed out connecting to server\n");
            (void) close_owned_fd(&client_fd);
            return -1;
        }

        descriptor.fd = client_fd;
        descriptor.events = POLLOUT;
        descriptor.revents = 0;
        poll_result = poll(&descriptor, 1U, remaining);

        if (poll_result < 0 && errno == EINTR) {
            continue;
        }

        if (poll_result <= 0) {
            fprintf(stderr, "poll(connect) failed or timed out: %s\n",
                    poll_result == 0 ? "timeout" : strerror(errno));
            (void) close_owned_fd(&client_fd);
            return -1;
        }

        if (getsockopt(client_fd,
                       SOL_SOCKET,
                       SO_ERROR,
                       &socket_error,
                       &error_length) < 0) {
            fprintf(stderr, "getsockopt(SO_ERROR) failed: %s\n",
                    strerror(errno));
            (void) close_owned_fd(&client_fd);
            return -1;
        }

        if (socket_error != 0) {
            fprintf(stderr, "connect(client) failed: %s\n",
                    strerror(socket_error));
            (void) close_owned_fd(&client_fd);
            return -1;
        }

        return client_fd;
    }
}

static int send_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    size_t remaining = length;
    struct timespec deadline;

    if (make_deadline(&deadline, IO_TIMEOUT_MS) < 0) {
        fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
        return -1;
    }

    while (remaining > 0U) {
        ssize_t sent = send(fd, cursor, remaining, MSG_NOSIGNAL);

        if (sent > 0) {
            cursor += (size_t) sent;
            remaining -= (size_t) sent;
            continue;
        }

        if (sent < 0 && errno == EINTR) {
            continue;
        }

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor;
            int timeout = remaining_timeout_ms(&deadline);
            int poll_result;

            if (timeout < 0) {
                fprintf(stderr, "clock_gettime failed: %s\n",
                        strerror(errno));
                return -1;
            }

            if (timeout == 0) {
                fprintf(stderr, "timed out sending client data\n");
                return -1;
            }

            descriptor.fd = fd;
            descriptor.events = POLLOUT;
            descriptor.revents = 0;
            poll_result = poll(&descriptor, 1U, timeout);

            if (poll_result < 0 && errno == EINTR) {
                continue;
            }

            if (poll_result <= 0) {
                fprintf(stderr, "poll(send) failed or timed out: %s\n",
                        poll_result == 0 ? "timeout" : strerror(errno));
                return -1;
            }

            continue;
        }

        fprintf(stderr, "send(client) failed: %s\n",
                sent == 0 ? "zero-byte send" : strerror(errno));
        return -1;
    }

    return 0;
}

static int receive_exact_with_server(
    struct minikv_server *server,
    int fd,
    const unsigned char *expected,
    size_t expected_length)
{
    unsigned char buffer[256];
    struct timespec deadline;
    size_t received_total = 0U;

    if (expected_length > sizeof(buffer) ||
        make_deadline(&deadline, IO_TIMEOUT_MS) < 0) {
        return -1;
    }

    while (received_total < expected_length) {
        ssize_t received;
        int remaining = remaining_timeout_ms(&deadline);
        int poll_result;

        if (remaining <= 0) {
            fprintf(stderr, "timed out waiting for server response\n");
            return -1;
        }

        poll_result = minikv_server_poll(
            server,
            remaining < 50 ? remaining : 50);

        if (poll_result != MINIKV_POLL_CONTINUE) {
            fprintf(stderr, "server poll returned %d\n", poll_result);
            return -1;
        }

        received = recv(
            fd,
            buffer + received_total,
            expected_length - received_total,
            0);

        if (received > 0) {
            received_total += (size_t) received;
            continue;
        }

        if (received == 0) {
            fprintf(stderr, "unexpected EOF while receiving response\n");
            return -1;
        }

        if (errno == EINTR ||
            errno == EAGAIN ||
            errno == EWOULDBLOCK) {
            continue;
        }

        fprintf(stderr, "recv(client) failed: %s\n", strerror(errno));
        return -1;
    }

    if (memcmp(buffer, expected, expected_length) != 0) {
        fprintf(stderr, "server response bytes did not match\n");
        return -1;
    }

    return 0;
}

static int expect_eof_without_data(
    struct minikv_server *server,
    int fd)
{
    unsigned char buffer[64];
    struct timespec deadline;

    if (make_deadline(&deadline, IO_TIMEOUT_MS) < 0) {
        return -1;
    }

    for (;;) {
        ssize_t received;
        int remaining = remaining_timeout_ms(&deadline);
        int poll_result;

        if (remaining <= 0) {
            fprintf(stderr, "timed out waiting for client EOF\n");
            return -1;
        }

        poll_result = minikv_server_poll(
            server,
            remaining < 50 ? remaining : 50);

        if (poll_result != MINIKV_POLL_CONTINUE) {
            fprintf(stderr, "server poll returned %d\n", poll_result);
            return -1;
        }

        received = recv(fd, buffer, sizeof(buffer), 0);

        if (received == 0) {
            return 0;
        }

        if (received > 0) {
            fprintf(stderr,
                    "received %zd unexpected bytes before EOF\n",
                    received);
            return -1;
        }

        if (errno == EINTR ||
            errno == EAGAIN ||
            errno == EWOULDBLOCK) {
            continue;
        }

        fprintf(stderr, "recv(client) failed: %s\n", strerror(errno));
        return -1;
    }
}

static int wait_for_connection_count(
    struct minikv_server *server,
    size_t expected_count,
    int timeout_ms)
{
    struct timespec deadline;

    if (make_deadline(&deadline, timeout_ms) < 0) {
        fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
        return -1;
    }

    while (minikv_server_connection_count(server) != expected_count) {
        int remaining = remaining_timeout_ms(&deadline);
        int poll_timeout;
        int poll_result;

        if (remaining < 0) {
            fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
            return -1;
        }

        if (remaining == 0) {
            fprintf(stderr,
                    "timed out waiting for connection count %zu; got %zu\n",
                    expected_count,
                    minikv_server_connection_count(server));
            return -1;
        }

        poll_timeout = remaining < 100 ? remaining : 100;
        poll_result = minikv_server_poll(server, poll_timeout);

        if (poll_result == MINIKV_POLL_ERROR) {
            fprintf(stderr, "server poll failed while waiting for cleanup\n");
            return -1;
        }

        if (poll_result == MINIKV_POLL_STOP) {
            fprintf(stderr, "unexpected server stop while waiting for cleanup\n");
            return -1;
        }
    }

    return 0;
}

static int test_basic_create_and_destroy(void)
{
    const struct minikv_server_options options = {
        "127.0.0.1",
        0U
    };
    struct minikv_server *server = NULL;
    int listener_fd;
    int event_loop_fd;
    int result = 1;

    if (minikv_server_create(&server, &options) < 0) {
        fprintf(stderr, "minikv_server_create failed: %s\n", strerror(errno));
        goto cleanup;
    }

    if (server == NULL) {
        fprintf(stderr, "minikv_server_create returned a NULL server\n");
        goto cleanup;
    }

    if (minikv_server_bound_port(server) == 0U) {
        fprintf(stderr, "server did not receive an ephemeral port\n");
        goto cleanup;
    }

    listener_fd = minikv_server_listener_fd(server);
    event_loop_fd = minikv_server_event_loop_fd(server);

    if (listener_fd < 0 || event_loop_fd < 0) {
        fprintf(stderr, "server returned an invalid borrowed fd\n");
        goto cleanup;
    }

    if (minikv_server_connection_count(server) != 0U) {
        fprintf(stderr, "new server has active connections\n");
        goto cleanup;
    }

    if (check_fd_flags("listener fd", listener_fd, true) < 0 ||
        check_fd_flags("event-loop fd", event_loop_fd, false) < 0) {
        goto cleanup;
    }

    minikv_server_destroy(server);
    server = NULL;

    if (expect_closed_fd("listener fd", listener_fd) < 0 ||
        expect_closed_fd("event-loop fd", event_loop_fd) < 0) {
        goto cleanup;
    }

    minikv_server_destroy(NULL);
    result = 0;

cleanup:
    if (server != NULL) {
        minikv_server_destroy(server);
    }

    return result;
}

static int test_accept_data_and_cleanup(void)
{
    static const char payload[] = "MiniKV M1 lifecycle test";
    const struct minikv_server_options options = {
        "127.0.0.1",
        0U
    };
    struct minikv_server *server = NULL;
    int clients[CLIENT_COUNT] = {-1, -1, -1};
    int accepted[CLIENT_COUNT];
    uint16_t port;
    size_t index;
    int poll_result;
    int result = 1;

    if (minikv_server_create(&server, &options) < 0) {
        fprintf(stderr, "minikv_server_create failed: %s\n", strerror(errno));
        goto cleanup;
    }

    port = minikv_server_bound_port(server);

    if (port == 0U) {
        fprintf(stderr, "server did not receive an ephemeral port\n");
        goto cleanup;
    }

    for (index = 0U; index < CLIENT_COUNT; index++) {
        clients[index] = connect_client(port);

        if (clients[index] < 0) {
            goto cleanup;
        }
    }

    poll_result = minikv_server_poll(server, EVENT_TIMEOUT_MS);

    if (poll_result != MINIKV_POLL_CONTINUE) {
        fprintf(stderr, "single accept poll returned %d\n", poll_result);
        goto cleanup;
    }

    if (minikv_server_connection_count(server) != CLIENT_COUNT) {
        fprintf(stderr,
                "single accept poll produced %zu connections, expected %u\n",
                minikv_server_connection_count(server),
                CLIENT_COUNT);
        goto cleanup;
    }

    for (index = 0U; index < CLIENT_COUNT; index++) {
        size_t previous;

        accepted[index] = minikv_server_connection_fd(server, index);

        if (accepted[index] < 0) {
            fprintf(stderr, "accepted fd %zu is invalid\n", index);
            goto cleanup;
        }

        for (previous = 0U; previous < index; previous++) {
            if (accepted[previous] == accepted[index]) {
                fprintf(stderr, "accepted fd %zu is duplicated\n", index);
                goto cleanup;
            }
        }

        if (check_fd_flags("accepted fd", accepted[index], true) < 0) {
            goto cleanup;
        }
    }

    for (index = 0U; index < CLIENT_COUNT; index++) {
        if (send_all(clients[index], payload, sizeof(payload) - 1U) < 0) {
            goto cleanup;
        }
    }

    poll_result = minikv_server_poll(server, EVENT_TIMEOUT_MS);

    if (poll_result != MINIKV_POLL_CONTINUE) {
        fprintf(stderr, "data drain poll returned %d\n", poll_result);
        goto cleanup;
    }

    if (minikv_server_connection_count(server) != CLIENT_COUNT) {
        fprintf(stderr, "data drain unexpectedly changed connection count\n");
        goto cleanup;
    }

    for (index = 0U; index < CLIENT_COUNT; index++) {
        if (close_owned_fd(&clients[index]) < 0) {
            goto cleanup;
        }
    }

    if (wait_for_connection_count(
            server,
            0U,
            CLEANUP_TIMEOUT_MS) < 0) {
        goto cleanup;
    }

    result = 0;

cleanup:
    for (index = 0U; index < CLIENT_COUNT; index++) {
        if (close_owned_fd(&clients[index]) < 0) {
            result = 1;
        }
    }

    if (server != NULL) {
        minikv_server_destroy(server);
    }

    return result;
}

static int test_invalid_address(void)
{
    const struct minikv_server_options invalid_options = {
        "999.999.999.999",
        0U
    };
    struct minikv_server *invalid_server = NULL;
    int result = 1;

    if (minikv_server_create(&invalid_server, &invalid_options) == 0) {
        fprintf(stderr, "invalid IPv4 address unexpectedly succeeded\n");
        goto cleanup;
    }

    if (invalid_server != NULL) {
        fprintf(stderr, "failed create returned a non-NULL server\n");
        goto cleanup;
    }

    result = 0;

cleanup:
    if (invalid_server != NULL) {
        minikv_server_destroy(invalid_server);
    }

    return result;
}

static int test_repeated_ephemeral_server(void)
{
    const struct minikv_server_options options = {
        "127.0.0.1",
        0U
    };
    struct minikv_server *first_server = NULL;
    struct minikv_server *second_server = NULL;
    int result = 1;

    if (minikv_server_create(&first_server, &options) < 0) {
        fprintf(stderr, "first ephemeral server failed: %s\n", strerror(errno));
        goto cleanup;
    }

    if (minikv_server_bound_port(first_server) == 0U) {
        fprintf(stderr, "first ephemeral server returned port zero\n");
        goto cleanup;
    }

    minikv_server_destroy(first_server);
    first_server = NULL;

    if (minikv_server_create(&second_server, &options) < 0) {
        fprintf(stderr, "second ephemeral server failed: %s\n", strerror(errno));
        goto cleanup;
    }

    if (minikv_server_bound_port(second_server) == 0U) {
        fprintf(stderr, "second ephemeral server returned port zero\n");
        goto cleanup;
    }

    result = 0;

cleanup:
    if (first_server != NULL) {
        minikv_server_destroy(first_server);
    }

    if (second_server != NULL) {
        minikv_server_destroy(second_server);
    }

    return result;
}

static int test_hook_arguments(void)
{
    const struct minikv_server_options options = {
        "127.0.0.1",
        0U
    };
    struct minikv_server *server = NULL;
    int result = 1;

    if (minikv_server_test_set_send_chunk_limit(NULL, 1U) != -1 ||
        minikv_server_test_force_next_send_would_block(NULL) != -1 ||
        minikv_server_test_set_output_limit(NULL, 1U) != -1) {
        fprintf(stderr, "NULL server test hook unexpectedly succeeded\n");
        goto cleanup;
    }

    if (minikv_server_create(&server, &options) < 0) {
        fprintf(stderr, "minikv_server_create failed: %s\n", strerror(errno));
        goto cleanup;
    }

    if (minikv_server_test_set_output_limit(server, 0U) != -1 ||
        minikv_server_test_set_output_limit(server, 14U) != 0 ||
        minikv_server_test_set_send_chunk_limit(server, 2U) != 0 ||
        minikv_server_test_set_send_chunk_limit(server, 0U) != 0 ||
        minikv_server_test_force_next_send_would_block(server) != 0) {
        fprintf(stderr, "valid test hook arguments failed\n");
        goto cleanup;
    }

    result = 0;

cleanup:
    minikv_server_destroy(server);
    return result;
}

static int test_forced_eagain_and_connection_reuse(void)
{
    static const unsigned char ping[] =
        "*1\r\n$4\r\nPING\r\n";
    static const unsigned char pong[] = "+PONG\r\n";
    const struct minikv_server_options options = {
        "127.0.0.1",
        0U
    };
    struct minikv_server *server = NULL;
    int client_fd = -1;
    int result = 1;

    if (minikv_server_create(&server, &options) < 0 ||
        minikv_server_test_set_send_chunk_limit(server, 2U) != 0) {
        fprintf(stderr, "server setup failed\n");
        goto cleanup;
    }

    client_fd = connect_client(minikv_server_bound_port(server));

    if (client_fd < 0 ||
        wait_for_connection_count(server, 1U, IO_TIMEOUT_MS) < 0 ||
        minikv_server_test_force_next_send_would_block(server) != 0 ||
        send_all(client_fd, ping, sizeof(ping) - 1U) < 0 ||
        receive_exact_with_server(
            server,
            client_fd,
            pong,
            sizeof(pong) - 1U) < 0) {
        goto cleanup;
    }

    if (send_all(client_fd, ping, sizeof(ping) - 1U) < 0 ||
        receive_exact_with_server(
            server,
            client_fd,
            pong,
            sizeof(pong) - 1U) < 0 ||
        minikv_server_connection_count(server) != 1U ||
        minikv_server_test_set_send_chunk_limit(server, 0U) != 0) {
        goto cleanup;
    }

    result = 0;

cleanup:
    if (close_owned_fd(&client_fd) < 0) {
        result = 1;
    }

    minikv_server_destroy(server);
    return result;
}

static int test_short_writes_and_pipeline(void)
{
    static const unsigned char echo[] =
        "*2\r\n$4\r\nECHO\r\n$12\r\nhello world!\r\n";
    static const unsigned char echo_reply[] =
        "$12\r\nhello world!\r\n";
    static const unsigned char pipeline[] =
        "*1\r\n$4\r\nPING\r\n"
        "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n"
        "*1\r\n$3\r\nGET\r\n"
        "*2\r\n$4\r\nPING\r\n$5\r\nworld\r\n";
    static const unsigned char pipeline_reply[] =
        "+PONG\r\n"
        "$5\r\nhello\r\n"
        "-ERR unknown command\r\n"
        "$5\r\nworld\r\n";
    static const unsigned char ping[] =
        "*1\r\n$4\r\nPING\r\n";
    static const unsigned char pong[] = "+PONG\r\n";
    const struct minikv_server_options options = {
        "127.0.0.1",
        0U
    };
    struct minikv_server *server = NULL;
    int client_fd = -1;
    int result = 1;

    if (minikv_server_create(&server, &options) < 0 ||
        minikv_server_test_set_send_chunk_limit(server, 2U) != 0) {
        goto cleanup;
    }

    client_fd = connect_client(minikv_server_bound_port(server));

    if (client_fd < 0 ||
        wait_for_connection_count(server, 1U, IO_TIMEOUT_MS) < 0 ||
        send_all(client_fd, echo, sizeof(echo) - 1U) < 0 ||
        receive_exact_with_server(
            server,
            client_fd,
            echo_reply,
            sizeof(echo_reply) - 1U) < 0 ||
        send_all(client_fd, ping, sizeof(ping) - 1U) < 0 ||
        receive_exact_with_server(
            server,
            client_fd,
            pong,
            sizeof(pong) - 1U) < 0 ||
        send_all(client_fd, pipeline, sizeof(pipeline) - 1U) < 0 ||
        receive_exact_with_server(
            server,
            client_fd,
            pipeline_reply,
            sizeof(pipeline_reply) - 1U) < 0) {
        goto cleanup;
    }

    result = 0;

cleanup:
    if (close_owned_fd(&client_fd) < 0) {
        result = 1;
    }

    minikv_server_destroy(server);
    return result;
}

static int test_output_limit_boundaries(void)
{
    static const unsigned char exact_request[] =
        "*2\r\n$4\r\nECHO\r\n$8\r\n12345678\r\n";
    static const unsigned char exact_reply[] =
        "$8\r\n12345678\r\n";
    static const unsigned char too_large_request[] =
        "*2\r\n$4\r\nECHO\r\n$9\r\n123456789\r\n";
    static const unsigned char ping[] =
        "*1\r\n$4\r\nPING\r\n";
    static const unsigned char pong[] = "+PONG\r\n";
    const struct minikv_server_options options = {
        "127.0.0.1",
        0U
    };
    struct minikv_server *server = NULL;
    int exact_client = -1;
    int rejected_client = -1;
    int healthy_client = -1;
    int result = 1;

    if (minikv_server_create(&server, &options) < 0 ||
        minikv_server_test_set_output_limit(server, 14U) != 0) {
        goto cleanup;
    }

    exact_client = connect_client(minikv_server_bound_port(server));

    if (exact_client < 0 ||
        wait_for_connection_count(server, 1U, IO_TIMEOUT_MS) < 0 ||
        send_all(
            exact_client,
            exact_request,
            sizeof(exact_request) - 1U) < 0 ||
        receive_exact_with_server(
            server,
            exact_client,
            exact_reply,
            sizeof(exact_reply) - 1U) < 0) {
        goto cleanup;
    }

    if (close_owned_fd(&exact_client) < 0 ||
        wait_for_connection_count(server, 0U, IO_TIMEOUT_MS) < 0) {
        goto cleanup;
    }

    rejected_client = connect_client(minikv_server_bound_port(server));

    if (rejected_client < 0 ||
        wait_for_connection_count(server, 1U, IO_TIMEOUT_MS) < 0 ||
        send_all(
            rejected_client,
            too_large_request,
            sizeof(too_large_request) - 1U) < 0 ||
        expect_eof_without_data(server, rejected_client) < 0 ||
        wait_for_connection_count(server, 0U, IO_TIMEOUT_MS) < 0) {
        goto cleanup;
    }

    healthy_client = connect_client(minikv_server_bound_port(server));

    if (healthy_client < 0 ||
        wait_for_connection_count(server, 1U, IO_TIMEOUT_MS) < 0 ||
        send_all(healthy_client, ping, sizeof(ping) - 1U) < 0 ||
        receive_exact_with_server(
            server,
            healthy_client,
            pong,
            sizeof(pong) - 1U) < 0) {
        goto cleanup;
    }

    result = 0;

cleanup:
    if (close_owned_fd(&exact_client) < 0 ||
        close_owned_fd(&rejected_client) < 0 ||
        close_owned_fd(&healthy_client) < 0) {
        result = 1;
    }

    minikv_server_destroy(server);
    return result;
}

static int test_destroy_with_partial_and_pending_connections(void)
{
    static const unsigned char partial[] = "*1\r\n$4\r\nPI";
    static const unsigned char ping[] = "*1\r\n$4\r\nPING\r\n";
    const struct minikv_server_options options = {
        "127.0.0.1",
        0U
    };
    struct minikv_server *server = NULL;
    int partial_client = -1;
    int pending_client = -1;
    int result = 1;

    if (minikv_server_create(&server, &options) < 0 ||
        minikv_server_test_set_send_chunk_limit(server, 1U) != 0 ||
        minikv_server_test_force_next_send_would_block(server) != 0) {
        goto cleanup;
    }

    partial_client = connect_client(minikv_server_bound_port(server));
    pending_client = connect_client(minikv_server_bound_port(server));

    if (partial_client < 0 ||
        pending_client < 0 ||
        wait_for_connection_count(server, 2U, IO_TIMEOUT_MS) < 0 ||
        send_all(partial_client, partial, sizeof(partial) - 1U) < 0 ||
        send_all(pending_client, ping, sizeof(ping) - 1U) < 0 ||
        minikv_server_poll(server, EVENT_TIMEOUT_MS) !=
            MINIKV_POLL_CONTINUE) {
        goto cleanup;
    }

    minikv_server_destroy(server);
    server = NULL;
    result = 0;

cleanup:
    if (close_owned_fd(&partial_client) < 0 ||
        close_owned_fd(&pending_client) < 0) {
        result = 1;
    }

    minikv_server_destroy(server);
    return result;
}

int main(void)
{
    static const struct test_case cases[] = {
        {"basic create and destroy", test_basic_create_and_destroy},
        {"accept, data, and cleanup", test_accept_data_and_cleanup},
        {"invalid address", test_invalid_address},
        {"repeated ephemeral server", test_repeated_ephemeral_server},
        {"test hook arguments", test_hook_arguments},
        {"forced EAGAIN and connection reuse",
         test_forced_eagain_and_connection_reuse},
        {"short writes and pipeline", test_short_writes_and_pipeline},
        {"output limit boundaries", test_output_limit_boundaries},
        {"destroy partial and pending connections",
         test_destroy_with_partial_and_pending_connections}
    };
    size_t index;
    int failures = 0;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); index++) {
        if (cases[index].run() != 0) {
            fprintf(stderr, "FAILED: %s\n", cases[index].name);
            failures++;
        } else {
            printf("PASSED: %s\n", cases[index].name);
        }
    }

    return failures == 0 ? 0 : 1;
}
