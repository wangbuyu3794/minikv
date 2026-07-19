#include "minikv/version.h"

#ifndef MINIKV_VERSION
#error "MINIKV_VERSION must be defined by the build system"
#endif

const char *minikv_version(void)
{
    return MINIKV_VERSION;
}
