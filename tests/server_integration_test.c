#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHILD_TIMEOUT_MS 3000
#define KILL_TIMEOUT_MS 2000
#define READINESS_TIMEOUT_MS 5000
#define IO_TIMEOUT_MS 5000
#define READINESS_CAPACITY 128U

struct child_process {
    pid_t pid;
    int stdout_fd;
    int stderr_fd;
    bool reaped;
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

static void child_process_init(struct child_process *child)
{
    child->pid = -1;
    child->stdout_fd = -1;
    child->stderr_fd = -1;
    child->reaped = true;
}

static void close_child_pipe_fd(int fd)
{
    if (fd > STDERR_FILENO) {
        (void) close(fd);
    }
}

static int set_parent_pipe_flags(int fd)
{
    int status_flags;
    int descriptor_flags;

    status_flags = fcntl(fd, F_GETFL);

    if (status_flags < 0 ||
        fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0) {
        return -1;
    }

    descriptor_flags = fcntl(fd, F_GETFD);

    if (descriptor_flags < 0 ||
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        return -1;
    }

    return 0;
}

static int spawn_child(
    struct child_process *child,
    const char *executable,
    char *const arguments[])
{
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    pid_t pid;
    int result = -1;

    if (pipe(stdout_pipe) < 0) {
        fprintf(stderr, "pipe(stdout) failed: %s\n", strerror(errno));
        goto cleanup;
    }

    if (pipe(stderr_pipe) < 0) {
        fprintf(stderr, "pipe(stderr) failed: %s\n", strerror(errno));
        goto cleanup;
    }

    pid = fork();

    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        goto cleanup;
    }

    if (pid == 0) {
        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            _exit(127);
        }

        close_child_pipe_fd(stdout_pipe[0]);
        close_child_pipe_fd(stdout_pipe[1]);
        close_child_pipe_fd(stderr_pipe[0]);
        close_child_pipe_fd(stderr_pipe[1]);

        execv(executable, arguments);
        _exit(127);
    }

    child->pid = pid;
    child->reaped = false;
    child->stdout_fd = stdout_pipe[0];
    stdout_pipe[0] = -1;
    child->stderr_fd = stderr_pipe[0];
    stderr_pipe[0] = -1;

    if (close_owned_fd(&stdout_pipe[1]) < 0 ||
        close_owned_fd(&stderr_pipe[1]) < 0) {
        goto cleanup;
    }

    if (set_parent_pipe_flags(child->stdout_fd) < 0 ||
        set_parent_pipe_flags(child->stderr_fd) < 0) {
        fprintf(stderr, "fcntl(parent pipe) failed: %s\n", strerror(errno));
        goto cleanup;
    }

    result = 0;

cleanup:
    (void) close_owned_fd(&stdout_pipe[0]);
    (void) close_owned_fd(&stdout_pipe[1]);
    (void) close_owned_fd(&stderr_pipe[0]);
    (void) close_owned_fd(&stderr_pipe[1]);
    return result;
}

static int wait_for_child_with_timeout(
    struct child_process *child,
    int timeout_ms,
    int *status)
{
    struct timespec deadline;

    if (child == NULL || status == NULL || child->reaped ||
        child->pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    if (make_deadline(&deadline, timeout_ms) < 0) {
        fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
        return -1;
    }

    for (;;) {
        pid_t waited = waitpid(child->pid, status, WNOHANG);

        if (waited == child->pid) {
            child->pid = -1;
            child->reaped = true;
            return 0;
        }

        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }

            fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
            return -1;
        }

        {
            int remaining = remaining_timeout_ms(&deadline);
            struct timespec pause_time = {0, 10000000L};

            if (remaining < 0) {
                fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
                return -1;
            }

            if (remaining == 0) {
                return 1;
            }

            if (remaining < 10) {
                pause_time.tv_nsec = (long) remaining * 1000000L;
            }

            if (nanosleep(&pause_time, NULL) < 0 && errno != EINTR) {
                fprintf(stderr, "nanosleep failed: %s\n", strerror(errno));
                return -1;
            }
        }
    }
}

static int terminate_child(
    struct child_process *child,
    int timeout_ms,
    int *status)
{
    if (child->reaped) {
        return 0;
    }

    if (kill(child->pid, SIGTERM) < 0 && errno != ESRCH) {
        fprintf(stderr, "kill(SIGTERM) failed: %s\n", strerror(errno));
        return -1;
    }

    return wait_for_child_with_timeout(child, timeout_ms, status);
}

static int kill_and_reap_child(
    struct child_process *child,
    int timeout_ms,
    int *status)
{
    if (child->reaped) {
        return 0;
    }

    if (kill(child->pid, SIGKILL) < 0 && errno != ESRCH) {
        fprintf(stderr, "kill(SIGKILL) failed: %s\n", strerror(errno));
        return -1;
    }

    return wait_for_child_with_timeout(child, timeout_ms, status);
}

static int cleanup_child(struct child_process *child)
{
    int status = 0;
    int cleanup_result = 0;
    int wait_result;

    if (close_owned_fd(&child->stdout_fd) < 0) {
        cleanup_result = -1;
    }

    if (close_owned_fd(&child->stderr_fd) < 0) {
        cleanup_result = -1;
    }

    if (!child->reaped) {
        wait_result = terminate_child(child, CHILD_TIMEOUT_MS, &status);

        if (wait_result != 0) {
            wait_result = kill_and_reap_child(
                child,
                KILL_TIMEOUT_MS,
                &status);

            if (wait_result != 0) {
                fprintf(stderr, "child could not be killed and reaped\n");
                cleanup_result = -1;
            }
        }
    }

    if (!child->reaped) {
        cleanup_result = -1;
    }

    return cleanup_result;
}

static int read_line_with_timeout(
    int fd,
    char *buffer,
    size_t capacity,
    int timeout_ms)
{
    struct timespec deadline;
    size_t length = 0U;

    if (buffer == NULL || capacity < 2U ||
        make_deadline(&deadline, timeout_ms) < 0) {
        return -1;
    }

    for (;;) {
        struct pollfd descriptor;
        int remaining = remaining_timeout_ms(&deadline);
        int poll_result;
        char byte;
        ssize_t received;

        if (remaining < 0) {
            fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
            return -1;
        }

        if (remaining == 0) {
            fprintf(stderr, "timed out waiting for readiness line\n");
            return -1;
        }

        descriptor.fd = fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;

        poll_result = poll(&descriptor, 1U, remaining);

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            fprintf(stderr, "poll(readiness) failed: %s\n", strerror(errno));
            return -1;
        }

        if (poll_result == 0) {
            fprintf(stderr, "timed out waiting for readiness line\n");
            return -1;
        }

        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            fprintf(stderr, "readiness pipe reported an error\n");
            return -1;
        }

        received = read(fd, &byte, 1U);

        if (received > 0) {
            if (byte == '\n') {
                buffer[length] = '\0';
                return 0;
            }

            if (length + 1U >= capacity) {
                fprintf(stderr, "readiness line is too long\n");
                return -1;
            }

            buffer[length++] = byte;
            continue;
        }

        if (received == 0) {
            fprintf(stderr, "readiness pipe closed before a complete line\n");
            return -1;
        }

        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }

        fprintf(stderr, "read(readiness) failed: %s\n", strerror(errno));
        return -1;
    }
}

static int parse_readiness_line(const char *line, uint16_t *port)
{
    static const char prefix[] = "MiniKV listening on 127.0.0.1:";
    const unsigned char *cursor;
    uint32_t value = 0U;

    if (line == NULL || port == NULL ||
        strncmp(line, prefix, sizeof(prefix) - 1U) != 0) {
        return -1;
    }

    cursor = (const unsigned char *) line + sizeof(prefix) - 1U;

    if (*cursor == '\0') {
        return -1;
    }

    while (*cursor != '\0') {
        uint32_t digit;

        if (*cursor < (unsigned char) '0' ||
            *cursor > (unsigned char) '9') {
            return -1;
        }

        digit = (uint32_t) (*cursor - (unsigned char) '0');

        if (value > (UINT16_MAX - digit) / 10U) {
            return -1;
        }

        value = value * 10U + digit;
        cursor++;
    }

    if (value == 0U) {
        return -1;
    }

    *port = (uint16_t) value;
    return 0;
}

static int connect_with_timeout(uint16_t port, int timeout_ms)
{
    struct sockaddr_in address;
    struct timespec deadline;
    int client_fd = -1;
    int status_flags;
    int descriptor_flags;
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

    if (make_deadline(&deadline, timeout_ms) < 0) {
        fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
        (void) close_owned_fd(&client_fd);
        return -1;
    }

    for (;;) {
        struct pollfd descriptor;
        socklen_t error_length = sizeof(connect_result);
        int remaining = remaining_timeout_ms(&deadline);
        int poll_result;

        if (remaining <= 0) {
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
                       &connect_result,
                       &error_length) < 0) {
            fprintf(stderr, "getsockopt(SO_ERROR) failed: %s\n",
                    strerror(errno));
            (void) close_owned_fd(&client_fd);
            return -1;
        }

        if (connect_result != 0) {
            fprintf(stderr, "connect(client) failed: %s\n",
                    strerror(connect_result));
            (void) close_owned_fd(&client_fd);
            return -1;
        }

        return client_fd;
    }
}

static int send_with_timeout(
    int fd,
    const void *buffer,
    size_t length,
    int timeout_ms)
{
    const unsigned char *cursor = buffer;
    size_t remaining_bytes = length;
    struct timespec deadline;

    if (make_deadline(&deadline, timeout_ms) < 0) {
        return -1;
    }

    while (remaining_bytes > 0U) {
        ssize_t sent = send(
            fd,
            cursor,
            remaining_bytes,
            MSG_NOSIGNAL);

        if (sent > 0) {
            cursor += (size_t) sent;
            remaining_bytes -= (size_t) sent;
            continue;
        }

        if (sent < 0 && errno == EINTR) {
            continue;
        }

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor;
            int remaining = remaining_timeout_ms(&deadline);
            int poll_result;

            if (remaining <= 0) {
                fprintf(stderr, "timed out sending client data\n");
                return -1;
            }

            descriptor.fd = fd;
            descriptor.events = POLLOUT;
            descriptor.revents = 0;
            poll_result = poll(&descriptor, 1U, remaining);

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

static void dump_available_pipe(const char *label, int fd)
{
    unsigned char buffer[512];

    if (fd < 0) {
        return;
    }

    fprintf(stderr, "----- %s -----\n", label);

    for (;;) {
        ssize_t received = read(fd, buffer, sizeof(buffer));

        if (received > 0) {
            (void) fwrite(buffer, 1U, (size_t) received, stderr);
            continue;
        }

        if (received < 0 && errno == EINTR) {
            continue;
        }

        if (received < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }

        break;
    }

    fputc('\n', stderr);
}

static int recv_exact_with_timeout(
    int fd,
    unsigned char *buffer,
    size_t length,
    int timeout_ms)
{
    struct timespec deadline;
    size_t offset = 0U;

    if (make_deadline(&deadline, timeout_ms) < 0) {
        return -1;
    }

    while (offset < length) {
        ssize_t received = recv(fd, buffer + offset, length - offset, 0);

        if (received > 0) {
            offset += (size_t) received;
            continue;
        }

        if (received == 0) {
            fprintf(stderr,
                    "unexpected EOF with %zu response bytes missing\n",
                    length - offset);
            return -1;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd descriptor;
            int remaining = remaining_timeout_ms(&deadline);
            int poll_result;

            if (remaining <= 0) {
                fprintf(stderr, "timed out receiving response\n");
                return -1;
            }

            descriptor.fd = fd;
            descriptor.events = POLLIN;
            descriptor.revents = 0;
            poll_result = poll(&descriptor, 1U, remaining);

            if (poll_result < 0 && errno == EINTR) {
                continue;
            }

            if (poll_result <= 0) {
                fprintf(stderr, "poll(recv) failed or timed out: %s\n",
                        poll_result == 0 ? "timeout" : strerror(errno));
                return -1;
            }

            continue;
        }

        fprintf(stderr, "recv(client) failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static int expect_bytes(
    int fd,
    const unsigned char *expected,
    size_t expected_length)
{
    unsigned char stack_buffer[512];
    unsigned char *actual = stack_buffer;
    int status = -1;

    if (expected_length > sizeof(stack_buffer)) {
        actual = malloc(expected_length);

        if (actual == NULL) {
            fprintf(stderr, "response comparison allocation failed\n");
            return -1;
        }
    }

    if (recv_exact_with_timeout(
            fd,
            actual,
            expected_length,
            IO_TIMEOUT_MS) == 0 &&
        memcmp(actual, expected, expected_length) == 0) {
        status = 0;
    } else if (status != 0) {
        fprintf(stderr, "response bytes did not match expected value\n");
    }

    if (actual != stack_buffer) {
        free(actual);
    }

    return status;
}

static int expect_eof(int fd)
{
    struct timespec deadline;
    unsigned char buffer[64];

    if (make_deadline(&deadline, IO_TIMEOUT_MS) < 0) {
        return -1;
    }

    for (;;) {
        ssize_t received = recv(fd, buffer, sizeof(buffer), 0);

        if (received == 0) {
            return 0;
        }

        if (received > 0) {
            fprintf(stderr,
                    "received %zd unexpected bytes before EOF\n",
                    received);
            return -1;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd descriptor;
            int remaining = remaining_timeout_ms(&deadline);
            int poll_result;

            if (remaining <= 0) {
                fprintf(stderr, "timed out waiting for EOF\n");
                return -1;
            }

            descriptor.fd = fd;
            descriptor.events = POLLIN | POLLHUP;
            descriptor.revents = 0;
            poll_result = poll(&descriptor, 1U, remaining);

            if (poll_result < 0 && errno == EINTR) {
                continue;
            }

            if (poll_result <= 0) {
                fprintf(stderr, "poll(EOF) failed or timed out: %s\n",
                        poll_result == 0 ? "timeout" : strerror(errno));
                return -1;
            }

            continue;
        }

        fprintf(stderr, "recv(EOF) failed: %s\n", strerror(errno));
        return -1;
    }
}

static int exchange(
    uint16_t port,
    const unsigned char *request,
    size_t request_length,
    const unsigned char *expected,
    size_t expected_length)
{
    int client_fd = connect_with_timeout(port, IO_TIMEOUT_MS);
    int status = -1;

    if (client_fd < 0 ||
        send_with_timeout(
            client_fd,
            request,
            request_length,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(client_fd, expected, expected_length) < 0) {
        goto cleanup;
    }

    status = 0;

cleanup:
    if (close_owned_fd(&client_fd) < 0) {
        status = -1;
    }

    return status;
}

static int exchange_then_eof(
    uint16_t port,
    const unsigned char *request,
    size_t request_length,
    const unsigned char *expected,
    size_t expected_length)
{
    int client_fd = connect_with_timeout(port, IO_TIMEOUT_MS);
    int status = -1;

    if (client_fd < 0 ||
        send_with_timeout(
            client_fd,
            request,
            request_length,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(client_fd, expected, expected_length) < 0 ||
        expect_eof(client_fd) < 0) {
        goto cleanup;
    }

    status = 0;

cleanup:
    if (close_owned_fd(&client_fd) < 0) {
        status = -1;
    }

    return status;
}

static int fragmented_exchange(
    uint16_t port,
    const unsigned char *request,
    size_t request_length,
    const size_t *parts,
    size_t part_count,
    const unsigned char *expected,
    size_t expected_length)
{
    int client_fd = connect_with_timeout(port, IO_TIMEOUT_MS);
    size_t offset = 0U;
    size_t index;
    int status = -1;

    if (client_fd < 0) {
        goto cleanup;
    }

    for (index = 0U; index < part_count; index++) {
        if (parts[index] > request_length - offset ||
            send_with_timeout(
                client_fd,
                request + offset,
                parts[index],
                IO_TIMEOUT_MS) < 0) {
            goto cleanup;
        }

        offset += parts[index];
    }

    if (offset != request_length ||
        expect_bytes(client_fd, expected, expected_length) < 0) {
        goto cleanup;
    }

    status = 0;

cleanup:
    if (close_owned_fd(&client_fd) < 0) {
        status = -1;
    }

    return status;
}

static int make_echo_request(
    const unsigned char *payload,
    size_t payload_length,
    unsigned char **out_request,
    size_t *out_length)
{
    char header[96];
    int header_length;
    size_t total;
    unsigned char *request;

    *out_request = NULL;
    *out_length = 0U;
    header_length = snprintf(
        header,
        sizeof(header),
        "*2\r\n$4\r\nECHO\r\n$%zu\r\n",
        payload_length);

    if (header_length < 0 ||
        (size_t) header_length >= sizeof(header) ||
        payload_length > SIZE_MAX - (size_t) header_length - 2U) {
        return -1;
    }

    total = (size_t) header_length + payload_length + 2U;
    request = malloc(total);

    if (request == NULL) {
        return -1;
    }

    memcpy(request, header, (size_t) header_length);

    if (payload_length > 0U) {
        memcpy(request + (size_t) header_length, payload, payload_length);
    }

    request[total - 2U] = (unsigned char) '\r';
    request[total - 1U] = (unsigned char) '\n';
    *out_request = request;
    *out_length = total;
    return 0;
}

static int make_bulk_reply(
    const unsigned char *payload,
    size_t payload_length,
    unsigned char **out_reply,
    size_t *out_length)
{
    char header[64];
    int header_length;
    size_t total;
    unsigned char *reply;

    *out_reply = NULL;
    *out_length = 0U;
    header_length = snprintf(
        header,
        sizeof(header),
        "$%zu\r\n",
        payload_length);

    if (header_length < 0 ||
        (size_t) header_length >= sizeof(header) ||
        payload_length > SIZE_MAX - (size_t) header_length - 2U) {
        return -1;
    }

    total = (size_t) header_length + payload_length + 2U;
    reply = malloc(total);

    if (reply == NULL) {
        return -1;
    }

    memcpy(reply, header, (size_t) header_length);

    if (payload_length > 0U) {
        memcpy(reply + (size_t) header_length, payload, payload_length);
    }

    reply[total - 2U] = (unsigned char) '\r';
    reply[total - 1U] = (unsigned char) '\n';
    *out_reply = reply;
    *out_length = total;
    return 0;
}

static int test_runtime(char *executable)
{
    static const unsigned char ping[] =
        "*1\r\n$4\r\nPING\r\n";
    static const unsigned char ping_lower[] =
        "*1\r\n$4\r\nping\r\n";
    static const unsigned char ping_mixed[] =
        "*1\r\n$4\r\nPiNg\r\n";
    static const unsigned char pong[] = "+PONG\r\n";
    static const unsigned char ping_message[] =
        "*2\r\n$4\r\nPING\r\n$5\r\nhello\r\n";
    static const unsigned char echo_normal[] =
        "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n";
    static const unsigned char echo_lower[] =
        "*2\r\n$4\r\necho\r\n$1\r\nx\r\n";
    static const unsigned char echo_mixed[] =
        "*2\r\n$4\r\nEcHo\r\n$1\r\ny\r\n";
    static const unsigned char echo_empty[] =
        "*2\r\n$4\r\nECHO\r\n$0\r\n\r\n";
    static const unsigned char hello_reply[] = "$5\r\nhello\r\n";
    static const unsigned char empty_reply[] = "$0\r\n\r\n";
    static const unsigned char binary_request[] = {
        '*', '2', '\r', '\n',
        '$', '4', '\r', '\n', 'E', 'C', 'H', 'O', '\r', '\n',
        '$', '5', '\r', '\n', 0x00, '\r', '\n', '\r', '\n',
        '\r', '\n'
    };
    static const unsigned char binary_reply[] = {
        '$', '5', '\r', '\n', 0x00, '\r', '\n', '\r', '\n',
        '\r', '\n'
    };
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
    static const unsigned char unknown_request[] =
        "*1\r\n$3\r\nGET\r\n";
    static const unsigned char unknown_reply[] =
        "-ERR unknown command\r\n";
    static const unsigned char ping_arity_request[] =
        "*3\r\n$4\r\nPING\r\n$1\r\na\r\n$1\r\nb\r\n";
    static const unsigned char ping_arity_reply[] =
        "-ERR wrong number of arguments for 'ping' command\r\n";
    static const unsigned char echo_arity_request[] =
        "*1\r\n$4\r\nECHO\r\n";
    static const unsigned char echo_arity_reply[] =
        "-ERR wrong number of arguments for 'echo' command\r\n";
    static const unsigned char protocol_error[] =
        "-ERR Protocol error\r\n";
    static const unsigned char malformed[] = "?\r\n";
    static const unsigned char bulk_shape[] = "$4\r\nPING\r\n";
    static const unsigned char nested_shape[] =
        "*2\r\n$4\r\nPING\r\n*0\r\n";
    static const unsigned char non_bulk_shape[] =
        "*2\r\n$4\r\nPING\r\n+arg\r\n";
    static const unsigned char null_bulk_shape[] =
        "*2\r\n$4\r\nPING\r\n$-1\r\n";
    static const unsigned char partial[] = "*1\r\n$4\r\nPI";
    static const size_t ping_header_parts[] = {1U, 3U, 10U};
    static const size_t bulk_length_parts[] = {14U, 1U, 1U, 9U};
    static const size_t payload_parts[] = {18U, 2U, 3U, 2U};
    static const size_t trailing_parts[] = {23U, 1U, 1U};
    struct child_process child;
    char readiness[READINESS_CAPACITY];
    char *arguments[] = {
        executable,
        "--bind",
        "127.0.0.1",
        "--port",
        "0",
        NULL
    };
    unsigned char *large_payload = NULL;
    unsigned char *large_request = NULL;
    unsigned char *large_reply = NULL;
    size_t large_request_length = 0U;
    size_t large_reply_length = 0U;
    size_t byte_parts[sizeof(ping) - 1U];
    size_t index;
    uint16_t port;
    int first_fd = -1;
    int second_fd = -1;
    int partial_fd = -1;
    int pending_fd = -1;
    int child_status = 0;
    int wait_result;
    int result = 1;

#define RUN_SCENARIO(name, expression)                                   \
    do {                                                                  \
        if ((expression) < 0) {                                           \
            fprintf(stderr, "FAILED: %s\n", (name));                      \
            goto cleanup;                                                 \
        }                                                                 \
        printf("PASSED: %s\n", (name));                                   \
    } while (0)

    child_process_init(&child);

    if (spawn_child(&child, executable, arguments) < 0) {
        goto cleanup;
    }

    if (read_line_with_timeout(
            child.stdout_fd,
            readiness,
            sizeof(readiness),
            READINESS_TIMEOUT_MS) < 0) {
        goto cleanup;
    }

    if (parse_readiness_line(readiness, &port) < 0) {
        fprintf(stderr, "invalid readiness line: '%s'\n", readiness);
        goto cleanup;
    }

    RUN_SCENARIO(
        "PING",
        exchange(
            port,
            ping,
            sizeof(ping) - 1U,
            pong,
            sizeof(pong) - 1U));
    RUN_SCENARIO(
        "lowercase PING",
        exchange(
            port,
            ping_lower,
            sizeof(ping_lower) - 1U,
            pong,
            sizeof(pong) - 1U));
    RUN_SCENARIO(
        "mixed-case PING",
        exchange(
            port,
            ping_mixed,
            sizeof(ping_mixed) - 1U,
            pong,
            sizeof(pong) - 1U));
    RUN_SCENARIO(
        "PING message",
        exchange(
            port,
            ping_message,
            sizeof(ping_message) - 1U,
            hello_reply,
            sizeof(hello_reply) - 1U));
    RUN_SCENARIO(
        "ECHO normal",
        exchange(
            port,
            echo_normal,
            sizeof(echo_normal) - 1U,
            hello_reply,
            sizeof(hello_reply) - 1U));
    RUN_SCENARIO(
        "lowercase ECHO",
        exchange(
            port,
            echo_lower,
            sizeof(echo_lower) - 1U,
            (const unsigned char *) "$1\r\nx\r\n",
            sizeof("$1\r\nx\r\n") - 1U));
    RUN_SCENARIO(
        "mixed-case ECHO",
        exchange(
            port,
            echo_mixed,
            sizeof(echo_mixed) - 1U,
            (const unsigned char *) "$1\r\ny\r\n",
            sizeof("$1\r\ny\r\n") - 1U));
    RUN_SCENARIO(
        "empty ECHO",
        exchange(
            port,
            echo_empty,
            sizeof(echo_empty) - 1U,
            empty_reply,
            sizeof(empty_reply) - 1U));
    RUN_SCENARIO(
        "binary ECHO",
        exchange(
            port,
            binary_request,
            sizeof(binary_request),
            binary_reply,
            sizeof(binary_reply)));

    for (index = 0U; index < sizeof(byte_parts) / sizeof(byte_parts[0]);
         index++) {
        byte_parts[index] = 1U;
    }

    RUN_SCENARIO(
        "bytewise request fragmentation",
        fragmented_exchange(
            port,
            ping,
            sizeof(ping) - 1U,
            byte_parts,
            sizeof(byte_parts) / sizeof(byte_parts[0]),
            pong,
            sizeof(pong) - 1U));
    RUN_SCENARIO(
        "header fragmentation",
        fragmented_exchange(
            port,
            ping,
            sizeof(ping) - 1U,
            ping_header_parts,
            sizeof(ping_header_parts) / sizeof(ping_header_parts[0]),
            pong,
            sizeof(pong) - 1U));
    RUN_SCENARIO(
        "bulk length fragmentation",
        fragmented_exchange(
            port,
            echo_normal,
            sizeof(echo_normal) - 1U,
            bulk_length_parts,
            sizeof(bulk_length_parts) / sizeof(bulk_length_parts[0]),
            hello_reply,
            sizeof(hello_reply) - 1U));
    RUN_SCENARIO(
        "payload fragmentation",
        fragmented_exchange(
            port,
            echo_normal,
            sizeof(echo_normal) - 1U,
            payload_parts,
            sizeof(payload_parts) / sizeof(payload_parts[0]),
            hello_reply,
            sizeof(hello_reply) - 1U));
    RUN_SCENARIO(
        "trailing CRLF fragmentation",
        fragmented_exchange(
            port,
            echo_normal,
            sizeof(echo_normal) - 1U,
            trailing_parts,
            sizeof(trailing_parts) / sizeof(trailing_parts[0]),
            hello_reply,
            sizeof(hello_reply) - 1U));
    RUN_SCENARIO(
        "single-write pipeline and response order",
        exchange(
            port,
            pipeline,
            sizeof(pipeline) - 1U,
            pipeline_reply,
            sizeof(pipeline_reply) - 1U));

    first_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (first_fd < 0 ||
        send_with_timeout(
            first_fd,
            ping,
            sizeof(ping) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(first_fd, pong, sizeof(pong) - 1U) < 0 ||
        send_with_timeout(
            first_fd,
            echo_normal,
            sizeof(echo_normal) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(
            first_fd,
            hello_reply,
            sizeof(hello_reply) - 1U) < 0 ||
        send_with_timeout(
            first_fd,
            pipeline,
            sizeof(pipeline) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(
            first_fd,
            pipeline_reply,
            sizeof(pipeline_reply) - 1U) < 0) {
        fprintf(stderr, "FAILED: multiple pipeline batches\n");
        goto cleanup;
    }

    printf("PASSED: multiple pipeline batches\n");
    if (close_owned_fd(&first_fd) < 0) {
        goto cleanup;
    }

    first_fd = connect_with_timeout(port, IO_TIMEOUT_MS);
    second_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (first_fd < 0 || second_fd < 0 ||
        send_with_timeout(
            first_fd,
            partial,
            sizeof(partial) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        send_with_timeout(
            second_fd,
            ping,
            sizeof(ping) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(second_fd, pong, sizeof(pong) - 1U) < 0 ||
        send_with_timeout(
            first_fd,
            (const unsigned char *) "NG\r\n",
            sizeof("NG\r\n") - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(first_fd, pong, sizeof(pong) - 1U) < 0) {
        fprintf(stderr, "FAILED: two-client parser isolation\n");
        goto cleanup;
    }

    printf("PASSED: two-client parser isolation\n");
    if (close_owned_fd(&first_fd) < 0 ||
        close_owned_fd(&second_fd) < 0) {
        goto cleanup;
    }

    first_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (first_fd < 0 ||
        send_with_timeout(
            first_fd,
            unknown_request,
            sizeof(unknown_request) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(
            first_fd,
            unknown_reply,
            sizeof(unknown_reply) - 1U) < 0 ||
        send_with_timeout(
            first_fd,
            ping,
            sizeof(ping) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(first_fd, pong, sizeof(pong) - 1U) < 0) {
        fprintf(stderr, "FAILED: unknown then PING\n");
        goto cleanup;
    }

    printf("PASSED: unknown then PING\n");
    if (close_owned_fd(&first_fd) < 0) {
        goto cleanup;
    }

    first_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (first_fd < 0 ||
        send_with_timeout(
            first_fd,
            ping_arity_request,
            sizeof(ping_arity_request) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(
            first_fd,
            ping_arity_reply,
            sizeof(ping_arity_reply) - 1U) < 0 ||
        send_with_timeout(
            first_fd,
            ping,
            sizeof(ping) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(first_fd, pong, sizeof(pong) - 1U) < 0) {
        fprintf(stderr, "FAILED: PING wrong arity then PING\n");
        goto cleanup;
    }

    printf("PASSED: PING wrong arity then PING\n");
    if (close_owned_fd(&first_fd) < 0) {
        goto cleanup;
    }

    first_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (first_fd < 0 ||
        send_with_timeout(
            first_fd,
            echo_arity_request,
            sizeof(echo_arity_request) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(
            first_fd,
            echo_arity_reply,
            sizeof(echo_arity_reply) - 1U) < 0 ||
        send_with_timeout(
            first_fd,
            ping,
            sizeof(ping) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(first_fd, pong, sizeof(pong) - 1U) < 0) {
        fprintf(stderr, "FAILED: ECHO wrong arity then PING\n");
        goto cleanup;
    }

    printf("PASSED: ECHO wrong arity then PING\n");
    if (close_owned_fd(&first_fd) < 0) {
        goto cleanup;
    }

    RUN_SCENARIO(
        "malformed RESP protocol error and EOF",
        exchange_then_eof(
            port,
            malformed,
            sizeof(malformed) - 1U,
            protocol_error,
            sizeof(protocol_error) - 1U));
    RUN_SCENARIO(
        "top-level Bulk protocol error and EOF",
        exchange_then_eof(
            port,
            bulk_shape,
            sizeof(bulk_shape) - 1U,
            protocol_error,
            sizeof(protocol_error) - 1U));
    RUN_SCENARIO(
        "nested element protocol error and EOF",
        exchange_then_eof(
            port,
            nested_shape,
            sizeof(nested_shape) - 1U,
            protocol_error,
            sizeof(protocol_error) - 1U));
    RUN_SCENARIO(
        "non-Bulk element protocol error and EOF",
        exchange_then_eof(
            port,
            non_bulk_shape,
            sizeof(non_bulk_shape) - 1U,
            protocol_error,
            sizeof(protocol_error) - 1U));
    RUN_SCENARIO(
        "null-Bulk element protocol error and EOF",
        exchange_then_eof(
            port,
            null_bulk_shape,
            sizeof(null_bulk_shape) - 1U,
            protocol_error,
            sizeof(protocol_error) - 1U));

    first_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (first_fd < 0 ||
        send_with_timeout(
            first_fd,
            ping,
            sizeof(ping) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        send_with_timeout(
            first_fd,
            malformed,
            sizeof(malformed) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(first_fd, pong, sizeof(pong) - 1U) < 0 ||
        expect_bytes(
            first_fd,
            protocol_error,
            sizeof(protocol_error) - 1U) < 0 ||
        expect_eof(first_fd) < 0) {
        fprintf(stderr, "FAILED: queued reply before protocol error\n");
        goto cleanup;
    }

    printf("PASSED: queued reply before protocol error\n");
    if (close_owned_fd(&first_fd) < 0) {
        goto cleanup;
    }

    first_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (first_fd < 0 ||
        send_with_timeout(
            first_fd,
            ping,
            sizeof(ping) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        shutdown(first_fd, SHUT_WR) < 0 ||
        expect_bytes(first_fd, pong, sizeof(pong) - 1U) < 0 ||
        expect_eof(first_fd) < 0) {
        fprintf(stderr, "FAILED: complete request half-close\n");
        goto cleanup;
    }

    printf("PASSED: complete request half-close\n");
    if (close_owned_fd(&first_fd) < 0) {
        goto cleanup;
    }

    first_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (first_fd < 0 ||
        send_with_timeout(
            first_fd,
            partial,
            sizeof(partial) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        shutdown(first_fd, SHUT_WR) < 0 ||
        expect_eof(first_fd) < 0) {
        fprintf(stderr, "FAILED: incomplete request half-close\n");
        goto cleanup;
    }

    printf("PASSED: incomplete request half-close\n");
    if (close_owned_fd(&first_fd) < 0) {
        goto cleanup;
    }

    large_payload = malloc(1024U * 1024U);

    if (large_payload == NULL) {
        fprintf(stderr, "large payload allocation failed\n");
        goto cleanup;
    }

    for (index = 0U; index < 1024U * 1024U; index++) {
        large_payload[index] = (unsigned char) (index % 251U);
    }

    if (make_echo_request(
            large_payload,
            1024U * 1024U,
            &large_request,
            &large_request_length) < 0 ||
        make_bulk_reply(
            large_payload,
            1024U * 1024U,
            &large_reply,
            &large_reply_length) < 0) {
        fprintf(stderr, "large message construction failed\n");
        goto cleanup;
    }

    first_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (first_fd < 0) {
        goto cleanup;
    }

    {
        int receive_buffer = 1024;

        if (setsockopt(
                first_fd,
                SOL_SOCKET,
                SO_RCVBUF,
                &receive_buffer,
                sizeof(receive_buffer)) < 0) {
            fprintf(stderr, "setsockopt(SO_RCVBUF) failed: %s\n",
                    strerror(errno));
            goto cleanup;
        }
    }

    if (send_with_timeout(
            first_fd,
            large_request,
            large_request_length,
            IO_TIMEOUT_MS) < 0) {
        goto cleanup;
    }

    second_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (second_fd < 0 ||
        send_with_timeout(
            second_fd,
            ping,
            sizeof(ping) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(second_fd, pong, sizeof(pong) - 1U) < 0) {
        fprintf(stderr, "FAILED: backpressure client isolation\n");
        goto cleanup;
    }

    printf("PASSED: backpressure client isolation\n");
    if (close_owned_fd(&second_fd) < 0) {
        goto cleanup;
    }

    if (expect_bytes(first_fd, large_reply, large_reply_length) < 0 ||
        send_with_timeout(
            first_fd,
            ping,
            sizeof(ping) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(first_fd, pong, sizeof(pong) - 1U) < 0) {
        fprintf(stderr, "FAILED: large ECHO and connection reuse\n");
        goto cleanup;
    }

    printf("PASSED: large ECHO and connection reuse\n");
    if (close_owned_fd(&first_fd) < 0) {
        goto cleanup;
    }

    first_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (first_fd < 0 || close_owned_fd(&first_fd) < 0) {
        goto cleanup;
    }

    second_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (second_fd < 0 ||
        send_with_timeout(
            second_fd,
            ping,
            sizeof(ping) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        expect_bytes(second_fd, pong, sizeof(pong) - 1U) < 0) {
        fprintf(stderr, "FAILED: disconnect isolation\n");
        goto cleanup;
    }

    printf("PASSED: disconnect isolation\n");
    if (close_owned_fd(&second_fd) < 0) {
        goto cleanup;
    }

    partial_fd = connect_with_timeout(port, IO_TIMEOUT_MS);
    pending_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (partial_fd < 0 || pending_fd < 0 ||
        send_with_timeout(
            partial_fd,
            partial,
            sizeof(partial) - 1U,
            IO_TIMEOUT_MS) < 0 ||
        send_with_timeout(
            pending_fd,
            large_request,
            large_request_length,
            IO_TIMEOUT_MS) < 0) {
        fprintf(stderr, "FAILED: shutdown connection setup\n");
        goto cleanup;
    }

    wait_result = terminate_child(
        &child,
        CHILD_TIMEOUT_MS,
        &child_status);

    if (wait_result != 0) {
        fprintf(stderr, "server did not stop before the deadline\n");
        goto cleanup;
    }

    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        fprintf(stderr, "server exited abnormally: status=%d\n", child_status);
        goto cleanup;
    }

    printf("PASSED: shutdown with partial and unread large response\n");
    result = 0;

cleanup:
    if (close_owned_fd(&first_fd) < 0 ||
        close_owned_fd(&second_fd) < 0 ||
        close_owned_fd(&partial_fd) < 0 ||
        close_owned_fd(&pending_fd) < 0) {
        result = 1;
    }

    free(large_payload);
    free(large_request);
    free(large_reply);

    if (result != 0 && !child.reaped) {
        int diagnostic_status = 0;

        wait_result = terminate_child(
            &child,
            CHILD_TIMEOUT_MS,
            &diagnostic_status);

        if (wait_result != 0) {
            (void) kill_and_reap_child(
                &child,
                KILL_TIMEOUT_MS,
                &diagnostic_status);
        }
    }

    if (result != 0) {
        dump_available_pipe("server stdout", child.stdout_fd);
        dump_available_pipe("server stderr", child.stderr_fd);
    }

    if (cleanup_child(&child) < 0) {
        result = 1;
    }

#undef RUN_SCENARIO
    return result;
}

static int run_cli_case(
    const char *name,
    char *executable,
    char *const arguments[],
    int expected_exit)
{
    struct child_process child;
    int child_status = 0;
    int wait_result;
    int result = 1;

    child_process_init(&child);

    if (spawn_child(&child, executable, arguments) < 0) {
        goto cleanup;
    }

    wait_result = wait_for_child_with_timeout(
        &child,
        CHILD_TIMEOUT_MS,
        &child_status);

    if (wait_result != 0) {
        fprintf(stderr, "%s did not exit before the deadline\n", name);
        goto cleanup;
    }

    if (!WIFEXITED(child_status)) {
        fprintf(stderr, "%s did not exit normally: status=%d\n",
                name,
                child_status);
        goto cleanup;
    }

    if (WEXITSTATUS(child_status) != expected_exit) {
        fprintf(stderr,
                "%s returned %d, expected %d\n",
                name,
                WEXITSTATUS(child_status),
                expected_exit);
        goto cleanup;
    }

    result = 0;

cleanup:
    if (cleanup_child(&child) < 0) {
        result = 1;
    }

    if (result == 0) {
        printf("PASSED: %s\n", name);
    }

    return result;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr,
                "usage: minikv_server_integration_test <minikv-server>\n");
        return 1;
    }

    if (test_runtime(argv[1]) != 0) {
        fprintf(stderr, "FAILED: runtime lifecycle\n");
        return 1;
    }

    printf("PASSED: runtime lifecycle\n");

    {
        char *arguments[] = {argv[1], "--port", "65536", NULL};

        if (run_cli_case(
                "port above range",
                argv[1],
                arguments,
                2) != 0) {
            return 1;
        }
    }

    {
        char *arguments[] = {argv[1], "--port", NULL};

        if (run_cli_case(
                "missing port value",
                argv[1],
                arguments,
                2) != 0) {
            return 1;
        }
    }

    {
        char *arguments[] = {
            argv[1],
            "--port",
            "0",
            "--port",
            "1",
            NULL
        };

        if (run_cli_case(
                "duplicate port",
                argv[1],
                arguments,
                2) != 0) {
            return 1;
        }
    }

    {
        char *arguments[] = {
            argv[1],
            "--bind",
            "127.0.0.1",
            "--bind",
            "127.0.0.1",
            NULL
        };

        if (run_cli_case(
                "duplicate bind",
                argv[1],
                arguments,
                2) != 0) {
            return 1;
        }
    }

    {
        char *arguments[] = {argv[1], "--unknown", NULL};

        if (run_cli_case(
                "unknown argument",
                argv[1],
                arguments,
                2) != 0) {
            return 1;
        }
    }

    {
        char *arguments[] = {argv[1], "--help", "--port", "0", NULL};

        if (run_cli_case(
                "help mixed with options",
                argv[1],
                arguments,
                2) != 0) {
            return 1;
        }
    }

    {
        char *arguments[] = {
            argv[1],
            "--bind",
            "999.999.999.999",
            "--port",
            "0",
            NULL
        };

        if (run_cli_case(
                "invalid bind address",
                argv[1],
                arguments,
                1) != 0) {
            return 1;
        }
    }

    {
        char *arguments[] = {argv[1], "--help", NULL};

        if (run_cli_case(
                "long help",
                argv[1],
                arguments,
                0) != 0) {
            return 1;
        }
    }

    {
        char *arguments[] = {argv[1], "-h", NULL};

        if (run_cli_case(
                "short help",
                argv[1],
                arguments,
                0) != 0) {
            return 1;
        }
    }

    return 0;
}
