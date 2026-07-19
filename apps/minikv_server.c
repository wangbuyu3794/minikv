#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "minikv/version.h"
#include "server_internal.h"

static void print_help(void)
{
    printf("Usage: minikv-server [--help|-h]\n");
    printf("       minikv-server [--bind <IPv4>] [--port <0-65535>]\n");
    printf("\n");
    printf("MiniKV educational key-value database, version %s\n",
           minikv_version());
    printf("\n");
    printf("Options:\n");
    printf("  --bind <IPv4>     Bind to a numeric IPv4 address.\n");
    printf("  --port <port>     Listen on port 0 through 65535.\n");
    printf("  -h, --help        Show this help message and exit.\n");
    printf("\n");
    printf("M1 accepts TCP connections but does not implement a protocol.\n");
}

static int parse_port(const char *text, uint16_t *port)
{
    uint32_t value = 0U;
    const unsigned char *cursor;

    if (text == NULL || text[0] == '\0' || port == NULL) {
        return -1;
    }

    cursor = (const unsigned char *) text;

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

    *port = (uint16_t) value;
    return 0;
}

static int usage_error(const char *message)
{
    fprintf(stderr, "minikv-server: %s\n", message);
    fprintf(stderr, "Try 'minikv-server --help' for more information.\n");
    return 2;
}

int main(int argc, char **argv)
{
    struct minikv_server_options options = {
        "127.0.0.1",
        6379U
    };
    bool bind_seen = false;
    bool port_seen = false;
    int index;

    if (argc == 2 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_help();
        return 0;
    }

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--bind") == 0) {
            if (bind_seen) {
                return usage_error("--bind may only be specified once");
            }

            if (index + 1 >= argc) {
                return usage_error("--bind requires an IPv4 address");
            }

            options.bind_address = argv[++index];
            bind_seen = true;
        } else if (strcmp(argv[index], "--port") == 0) {
            if (port_seen) {
                return usage_error("--port may only be specified once");
            }

            if (index + 1 >= argc) {
                return usage_error("--port requires a value");
            }

            if (parse_port(argv[++index], &options.port) < 0) {
                return usage_error("port must be an integer from 0 to 65535");
            }

            port_seen = true;
        } else {
            fprintf(stderr,
                    "minikv-server: unknown argument '%s'\n",
                    argv[index]);
            fprintf(stderr,
                    "Try 'minikv-server --help' for more information.\n");
            return 2;
        }
    }

    return minikv_server_run(&options);
}
