# Build Tips

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
