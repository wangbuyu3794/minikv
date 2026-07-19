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
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHILD_TIMEOUT_MS 3000
#define KILL_TIMEOUT_MS 2000
#define READINESS_TIMEOUT_MS 5000
#define IO_TIMEOUT_MS 2000
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

static int test_runtime(char *executable)
{
    static const char payload[] = "MiniKV M1 integration test";
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
    uint16_t port;
    int client_fd = -1;
    int child_status = 0;
    int wait_result;
    int result = 1;

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

    client_fd = connect_with_timeout(port, IO_TIMEOUT_MS);

    if (client_fd < 0) {
        goto cleanup;
    }

    if (send_with_timeout(
            client_fd,
            payload,
            sizeof(payload) - 1U,
            IO_TIMEOUT_MS) < 0) {
        goto cleanup;
    }

    if (close_owned_fd(&client_fd) < 0) {
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

    result = 0;

cleanup:
    if (close_owned_fd(&client_fd) < 0) {
        result = 1;
    }

    if (cleanup_child(&child) < 0) {
        result = 1;
    }

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
