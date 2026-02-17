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

## Running Tests

See `AGENTS.md` for build and test commands.
