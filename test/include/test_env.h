#pragma once

// Provide setenv/unsetenv on Windows when missing (MSVC).
// Tests call setenv/unsetenv directly; on POSIX this header is a no-op.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdlib>
#include <cstring>

#include <windows.h>

static inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && getenv(name))
        return 0;
    BOOL ok = SetEnvironmentVariableA(name, value);
    return ok ? 0 : -1;
}

static inline int unsetenv(const char* name) {
    BOOL ok = SetEnvironmentVariableA(name, NULL);
    return ok ? 0 : -1;
}
#endif // _WIN32
