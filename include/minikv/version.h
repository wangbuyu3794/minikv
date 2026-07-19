#ifndef MINIKV_VERSION_H_INCLUDED
#define MINIKV_VERSION_H_INCLUDED

/*
 * Returns the MiniKV version for this build.
 *
 * The returned string has static storage duration. Callers must not modify or
 * free it.
 */
const char *minikv_version(void);

#endif
