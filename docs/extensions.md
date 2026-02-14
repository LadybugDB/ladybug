# Working with Extensions

## Available Extensions

The following extensions are available:
- `httpfs` - HTTP file system support
- `duckdb` - DuckDB integration
- `json` - JSON data type and functions
- `postgres` - PostgreSQL connector
- `sqlite` - SQLite connector
- `fts` - Full-text search
- `delta` - Delta Lake format
- `iceberg` - Apache Iceberg format
- `azure` - Azure storage
- `unity_catalog` - Unity Catalog integration
- `vector` - Vector search operations
- `neo4j` - Neo4j connector
- `algo` - Graph algorithms
- `llm` - LLM integration

## Building Extensions

```bash
# Build all extensions
make extension-build

# Build specific extensions
EXTENSION_LIST=vector,json make extension-build

# Debug build with extensions
make extension-debug

# Release build with extensions
make extension-release
```

## Testing Extensions

```bash
# Build and run all extension tests
make extension-test-build
make extension-test

# Build and test specific extension
EXTENSION_LIST=vector make extension-test-build
E2E_TEST_FILES_DIRECTORY=extension/vector/test/test_files ./build/relwithdebinfo/test/runner/e2e_test --gtest_filter="insert.InsertToNonEmpty"
```

## Static Linking

```bash
# Build with statically linked extensions
EXTENSION_LIST=vector,json EXTENSION_STATIC_LINK_LIST=vector,json make extension-test-static-build
make extension-static-link-test
```

## Coverage Testing

```bash
make extension-lcov-build
make extension-lcov
```

## Clean Extension Builds

```bash
make clean-extension
```
