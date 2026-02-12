## Building and testing specific extensions

The following will build only the `vector` extension and run only a specific test

```
EXTENSION_LIST=vector make extension-test-build
E2E_TEST_FILES_DIRECTORY=extension/vector/test/test_files ./build/relwithdebinfo/test/runner/e2e_test --gtest_filter="insert.InsertToNonEmpty"
```
