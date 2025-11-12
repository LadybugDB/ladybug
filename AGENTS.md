Use

make GEN=Ninja python

to compile

For debug builds, add -debug suffix to Makefile targets.

Python is an important surface area to test. If you make significant changes use

make GEN=Ninja pytest-venv 

to test.

Prefer using uv over pip for venv management.

## Changing the grammar

source: src/antlr4/Cypher.g4
generated: scripts/antrl4/Cypher.g4 (via keywordhandler.py)
generated: third_party/antlr4_cypher/cypher_parser.{cpp,h}

Running make will autogenerate, but manual generation can be done via

(cd scripts/antlr4 && cmake -D ROOT_DIR=../.. -P generate_grammar.cmake)

## Building and testing specific extensions

The following will build only the `vector` extension and run only a specific test

```
EXTENSION_LIST=vector make extension-test-build
E2E_TEST_FILES_DIRECTORY=extension/vector/test/test_files ./build/relwithdebinfo/test/runner/e2e_test --gtest_filter="insert.InsertToNonEmpty"
```
