#pragma once

// Provide setenv/unsetenv on Windows when missing (MSVC).
// Tests call setenv/unsetenv directly; on POSIX this header is a no-op.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdlib>
#include <cstring>
#include <string>

// NOTE: Do not use SetEnvironmentVariableA here. It only updates the OS
// environment block, which getenv() does not read: the CRT keeps its own
// copy in _environ. _putenv/_putenv_s update both, so getenv() sees them.
static inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && getenv(name))
        return 0;
    return _putenv_s(name, value);
}

static inline int unsetenv(const char* name) {
    // "NAME=" removes the variable from the CRT block (and the OS block).
    std::string entry = std::string(name) + "=";
    return _putenv(entry.c_str());
}
#endif // _WIN32
