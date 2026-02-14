# C++ Style Guidelines

## Naming Conventions

- **Classes/Structs**: `PascalCase` (e.g., `DatabaseManager`, `ClientContext`)
- **Functions**: `camelCase` (e.g., `getTableCatalogEntry`, `prepareWithParams`)
- **Variables**: `camelCase` (e.g., `bufferPoolSize`, `attachedDatabases`)
- **Private members**: Same as variables, no special prefix
- **Constants/Enums**: `UPPER_SNAKE_CASE` for macros, `PascalCase` for enum types
- **Namespaces**: `lbug` as root, then module name (e.g., `lbug::main`, `lbug::storage`)

## File Organization

- Headers: `src/include/<module>/<filename>.h`
- Source: `src/<module>/<filename>.cpp`
- Tests: `test/<category>/<filename>_test.cpp`

## Include Order

Enforced by clang-format:

1. C system headers (priority 20)
2. C++ system headers (priority 30)
3. Third-party libraries with `<>` (priority 40)
4. Project headers with `"src/` (priority 50)
5. Other local headers with `"` (priority 40)

## Imports Pattern

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

## Assertions

- Use `KU_ASSERT(condition)` for debug assertions
- Use `KU_ASSERT_UNCONDITIONAL(condition)` for always-on assertions
- Use `KU_UNREACHABLE` for unreachable code paths
- Use `KU_UNUSED(expr)` to mark intentionally unused variables

## Error Handling

- Throw exceptions for error conditions (e.g., `RuntimeException`, `BinderException`)
- Use `[[maybe_unused]]` attribute for unused parameters
- Use `[[unlikely]]` for cold paths

## Type Safety

- Prefer `std::unique_ptr` for ownership
- Use `explicit` for single-argument constructors
- Use `override` for virtual function overrides
- Pass by value for small types, const reference for large types
