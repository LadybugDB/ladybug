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
make extension-release        # Release build with extensions
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

### C++ Style Guidelines

#### Naming Conventions
- **Classes/Structs**: `PascalCase` (e.g., `DatabaseManager`, `ClientContext`)
- **Functions**: `camelCase` (e.g., `getTableCatalogEntry`, `prepareWithParams`)
- **Variables**: `camelCase` (e.g., `bufferPoolSize`, `attachedDatabases`)
- **Private members**: Same as variables, no special prefix
- **Constants/Enums**: `UPPER_SNAKE_CASE` for macros, `PascalCase` for enum types
- **Namespaces**: `lbug` as root, then module name (e.g., `lbug::main`, `lbug::storage`)

#### File Organization
- Headers: `src/include/<module>/<filename>.h`
- Source: `src/<module>/<filename>.cpp`
- Tests: `test/<category>/<filename>_test.cpp`

#### Include Order (enforced by clang-format)
1. C system headers (priority 20)
2. C++ system headers (priority 30)
3. Third-party libraries with `<>` (priority 40)
4. Project headers with `"src/` (priority 50)
5. Other local headers with `"` (priority 40)

#### Imports Pattern
```cpp
// C++ system headers first
#include <format>
#include <memory>
#include <string>

// Third-party libraries
#include "spdlog/spdlog.h"

// Project headers (use full path from src/)
#include "main/client_context.h"
#include "storage/storage_manager.h"
```

#### Assertions
- Use `KU_ASSERT(condition)` for debug assertions
- Use `KU_ASSERT_UNCONDITIONAL(condition)` for always-on assertions
- Use `KU_UNREACHABLE` for unreachable code paths
- Use `KU_UNUSED(expr)` to mark intentionally unused variables

#### Error Handling
- Throw exceptions for error conditions (e.g., `RuntimeException`, `BinderException`)
- Use `[[maybe_unused]]` attribute for unused parameters
- Use `[[unlikely]]` for cold paths

#### Type Safety
- Prefer `std::unique_ptr` for ownership
- Use `explicit` for single-argument constructors
- Use `override` for virtual function overrides
- Pass by value for small types, const reference for large types

## Testing Patterns

### Unit Test Structure
```cpp
#include "test_helper/test_helper.h"
#include <gtest/gtest.h>

namespace lbug {
namespace testing {

class MyTest : public DBTest {
    void SetUp() override {
        BaseGraphTest::SetUp();
        // Test setup
    }
};

TEST_F(MyTest, TestCaseName) {
    // Test implementation
}

} // namespace testing
} // namespace lbug
```

### Test Categories
- `test/runner/` - End-to-end tests
- `test/storage/` - Storage layer tests
- `test/transaction/` - Transaction tests
- `test/api/` - API tests
- `test/c_api/` - C API tests
- `test/binder/` - Query binder tests
- `test/planner/` - Query planner tests
- `test/optimizer/` - Query optimizer tests

## Additional Documentation

- `docs/build_tips.md` - Build tips and configuration options
- `docs/python.md` - Python development guide
- `docs/shell.md` - Shell development guide
- `docs/extensions.md` - Working with extensions
- `docs/grammar.md` - Editing Cypher grammar
