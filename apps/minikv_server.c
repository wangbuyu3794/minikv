#include <stdio.h>
#include <string.h>

#include "minikv/version.h"

static void print_help(void)
{
    printf("Usage: minikv-server [--help|-h]\n");
    printf("\n");
    printf("MiniKV educational key-value database, version %s\n",
           minikv_version());
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help  Show this help message and exit.\n");
    printf("\n");
    printf("The M0 skeleton does not start a database server.\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_help();
        return 0;
    }

    if (argc == 1) {
        fprintf(stderr,
                "minikv-server: the M0 skeleton cannot start a server\n");
        fprintf(stderr, "Try 'minikv-server --help' for more information.\n");
        return 1;
    }

    fprintf(stderr, "minikv-server: unknown argument '%s'\n", argv[1]);
    fprintf(stderr, "Try 'minikv-server --help' for more information.\n");
    return 2;
}
