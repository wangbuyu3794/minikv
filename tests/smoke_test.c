#include <stdio.h>
#include <string.h>

#include "minikv/version.h"

#ifndef MINIKV_EXPECTED_VERSION
#error "MINIKV_EXPECTED_VERSION must be defined by the build system"
#endif

int main(void)
{
    const char *version = minikv_version();

    if (version == NULL) {
        fprintf(stderr, "smoke test failed: version pointer is NULL\n");
        return 1;
    }

    if (version[0] == '\0') {
        fprintf(stderr, "smoke test failed: version string is empty\n");
        return 1;
    }

    if (strcmp(version, MINIKV_EXPECTED_VERSION) != 0) {
        fprintf(stderr,
                "smoke test failed: expected version %s, got %s\n",
                MINIKV_EXPECTED_VERSION,
                version);
        return 1;
    }

    printf("smoke test passed: MiniKV version %s\n", version);
    return 0;
}
