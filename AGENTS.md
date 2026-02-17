# Ladybug Agent Guidelines

## Build Commands

Prefer Ninja over make when possible.

```bash
# Release build (fastest, for production/testing)
make release

# Debug build (with debug symbols)
make debug

# RelWithDebInfo (recommended for testing with stack traces)
make relwithdebinfo

# Full build with all components
make all

# Build specific components
make python        # Python API
make java          # Java API
make nodejs        # Node.js API
make shell         # Shell CLI
make benchmark     # Benchmarks
make example       # Examples

# Build with extensions
make extension-build          # Build all extensions
make extension-debug          # Debug build with extensions
make extension-release       # Release build with extensions
```

### Windows (PowerShell, without make)

Requires CMake and Ninja in PATH (e.g. from LLVM or Visual Studio).

```powershell
# RelWithDebInfo (generates compile_commands.json for clangd/MCP)
cmake -B build/relwithdebinfo -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo .
cmake --build build/relwithdebinfo --config RelWithDebInfo

# Release
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release .
cmake --build build/release --config Release

# Debug
cmake -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug .
cmake --build build/debug --config Debug
```

## Test Commands

```bash
# Build tests (RelWithDebInfo)
make test-build

# Run all tests
make test

# Build and run tests (Release which is faster to build)
make test-build-release

# Run specific test with gtest filter
E2E_TEST_FILES_DIRECTORY=test/test_files build/release/test/runner/e2e_test --gtest_filter="*merge_tinysnb.Merge*"

# Extension tests
make extension-test-build
make extension-test

# Language-specific tests
make pytest           # Python tests
make javatest         # Java tests
make nodejstest       # Node.js tests
make rusttest         # Rust tests
make wasmtest         # WASM tests
```

## Code Style

### Formatting

Use clang-format-18:

```bash
python3 scripts/run-clang-format.py --clang-format-executable /usr/bin/clang-format-18 -r <dirs>
```

### Linting

```bash
make tidy             # Run clang-tidy checks
make tidy-analyzer    # Run analyzer-specific tidy checks
make clangd-diagnostics  # Get clangd diagnostics
```

## Additional Documentation

- `docs/build_tips.md` - Build tips and configuration options
- `docs/cpp_style.md` - C++ style guidelines
- `docs/testing.md` - Testing patterns and guidelines
- `docs/python.md` - Python development guide
- `docs/shell.md` - Shell development guide
- `docs/extensions.md` - Working with extensions
- `docs/grammar.md` - Editing Cypher grammar
