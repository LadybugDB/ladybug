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

# Rows per node group and maximum column segment size in bytes (both log2)
make release NODE_GROUP_SIZE_LOG2=17
make release MAX_SEGMENT_SIZE_LOG2=18

# Rel table storage direction when CREATE REL TABLE sets no storage_direction (BOTH, FWD or BWD)
make release DEFAULT_REL_STORAGE_DIRECTION=BOTH

# Buffer manager on malloc instead of mmap'd virtual memory regions (always on for WebAssembly and Swift)
make release BM_MALLOC=1

# Single-threaded engine
make release SINGLE_THREADED=1
```

## Benchmarking Against Release Builds

Release libraries are built by `precompiled-bin-workflow.yml` with `make GEN=Ninja`. A local build only compares with a release when it uses the same compiler and settings:

| Release | Compiler | Settings |
|---|---|---|
| Linux x86_64 and aarch64, compat: shared library, CLI, and the static library the Python wheels link | `gcc-toolset-13` in a `manylinux_2_28` container | `LDFLAGS=-static-libstdc++` |
| Linux x86_64 and aarch64, perf: static library for bindings such as Node.js and Java | `clang-21` from apt.llvm.org on Ubuntu 24.04 | `CC=clang-21 CXX=clang++-21` |
| macOS x86_64 and arm64 | the runner's Apple Clang | `MACOSX_DEPLOYMENT_TARGET=13.3` |
| Windows x64 and ARM64 | MSVC | |

On every platform, start from an empty `build/release` (`rm -rf build/release`, or `BUILD_PATH=<dir>` for a new one). `make test-build-release` and `make all` leave `BUILD_TESTS=TRUE` (the former also `ENABLE_BACKTRACES=TRUE`) in its cache, where a later `make python` keeps it; `BUILD_TESTS` exports every symbol, and such a build used 2–5% more instructions on Ubuntu 24.04 x86_64. CMake also reads `CC`, `CXX`, `CFLAGS` and `CXXFLAGS` only when it first configures a directory.

To approximate the Linux x86_64 compat release with a distribution GCC, turn off the hardening it enables by default and `gcc-toolset-13` does not. Ubuntu 24.04's GCC 13 on x86_64 enables `-fstack-protector-strong`, `-fstack-clash-protection`, `-fcf-protection=full` and `_FORTIFY_SOURCE=3`, which cost up to 12% more instructions on LDBC q30. The flags below are for x86_64; aarch64 was not checked:

```bash
git submodule update --init tools/python_api
rm -rf build/release
export CFLAGS="-fno-stack-protector -fcf-protection=none -fno-stack-clash-protection -U_FORTIFY_SOURCE"
export CXXFLAGS="$CFLAGS"
CC=gcc-13 CXX=g++-13 GEN=Ninja make python
```

Check the result: `objdump -d tools/python_api/build/ladybug/_lbug*.so | grep -c endbr64` should be in single digits, and `nm -D --defined-only tools/python_api/build/ladybug/_lbug*.so | wc -l` about 5,000. Built this way on Ubuntu 24.04 x86_64, single-threaded instruction counts matched the 0.21.1 Linux x86_64 wheel within 0.6% on ten LDBC SNB SF1 queries.

Wall time can still differ by several percent between two builds of the same code, most visibly on queries of a few milliseconds, so compare instruction counts (`perf stat -e instructions:u` on Linux) where you can.
