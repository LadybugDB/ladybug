# Build Tips

## Minimum Toolchain Requirements

- **C++20**: the codebase requires C++20 (e.g. `std::atomic_ref` in `src/c_api`). Pass `-DCMAKE_CXX_STANDARD=20 -DCMAKE_CXX_STANDARD_REQUIRED=ON` when configuring with CMake.
- **macOS: Xcode 16+ / macOS 15+ (libc++ 17+)**: `std::atomic_ref` was only implemented in libc++ 17 (LLVM 17). Xcode 15.4 ships libc++ 16, so builds fail with `error: no member named 'atomic_ref' in namespace 'std'`. On GitHub Actions, use `macos-15` (or newer) runners.
- **Windows / Linux**: any C++20-compliant compiler works (MSVC, GCC, Clang).

## General Build Options

- Use release builds for fast runtimes
- Use relwithdebinfo when you need stack traces
- Set `TEST_JOBS=10` (default) or adjust for parallel test execution
- Use `EXTRA_CMAKE_FLAGS` for additional cmake options

## CMake Build Types

```bash
# Release - optimized for performance
make release

# Debug - includes debug symbols, assertions enabled
make debug

# RelWithDebInfo - optimized with debug symbols
make relwithdebinfo
```

## Sanitizers

```bash
# Address sanitizer
make release ASAN=1

# Thread sanitizer
make release TSAN=1

# Undefined behavior sanitizer
make release UBSAN=1
```

## Build Configurations

```bash
# Runtime checks
make debug RUNTIME_CHECKS=1

# Treat warnings as errors
make release WERROR=1

# Link-time optimization
make release LTO=1

# Page size configuration
make release PAGE_SIZE_LOG2=12

# Vector capacity configuration
make release VECTOR_CAPACITY_LOG2=11
```
