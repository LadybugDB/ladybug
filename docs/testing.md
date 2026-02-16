# Testing Guide

## Unit Test Structure

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

## Test Categories

- `test/runner/` - End-to-end tests
- `test/storage/` - Storage layer tests
- `test/transaction/` - Transaction tests
- `test/api/` - API tests
- `test/c_api/` - C API tests
- `test/binder/` - Query binder tests
- `test/planner/` - Query planner tests
- `test/optimizer/` - Query optimizer tests

## Node.js API

Tests live in `tools/nodejs_api/test/` and use the Node.js built-in test runner (`node --test`). Run with `npm test` from `tools/nodejs_api/`.

For guidelines on writing and reviewing these tests, see [Node.js API — Testing Guide](../tools/nodejs_api/docs/nodejs_testing.md).

## Running Tests

See `AGENTS.md` for build and test commands.
