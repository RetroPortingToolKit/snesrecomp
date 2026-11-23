/* setenv()/unsetenv() for the C harnesses on Windows (MinGW), where the CRT
 * only offers _putenv_s. An empty value removes the variable there, so
 * unsetenv maps onto it. POSIX hosts use their own declarations. */
#ifndef SNESRECOMP_TESTS_POSIX_ENV_COMPAT_H
#define SNESRECOMP_TESTS_POSIX_ENV_COMPAT_H

#include <stdlib.h>

#ifdef _WIN32
static inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
static inline int unsetenv(const char *name) {
    return _putenv_s(name, "") == 0 ? 0 : -1;
}
#endif

#endif
