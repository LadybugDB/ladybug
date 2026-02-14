# Shell Development Guide

## Building the Shell

```bash
make shell           # Build shell
make shell-debug     # Debug shell build
```

The shell is built with `duckdb` and `json` extensions statically linked.

## Testing

```bash
make shell-test      # Run shell tests
```

Shell tests are located in `tools/shell/test/`.

## Running the Shell

```bash
# After building
./build/release/tools/shell/lbug

# Or with debug build
./build/debug/tools/shell/lbug
```

## Shell Features

The Ladybug shell supports:
- Interactive Cypher query execution
- Tab completion
- History navigation
- Output formatting options
