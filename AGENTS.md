Prefer Ninja over make
Prefer uv over pip for venv

make shell  # for tasks involving lbug shell cli
make python # for tasks involving python

Use release builds for fast builds/run times.
Use relwithdebinfo if stack traces are desired

## Running C++ tests

make test-build-release
E2E_TEST_FILES_DIRECTORY=test/test_files build/release/test/runner/e2e_test --gtest_filter="*merge_tinysnb.Merge*"

## More docs

docs/extensions.md on how to work with extensions
docs/grammar.md on how to edit Cypher grammar

