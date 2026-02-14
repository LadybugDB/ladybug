# Python Development Guide

## Building the Python API

```bash
make python          # Build Python API
make python-debug    # Debug build
```

## Testing

```bash
make pytest          # Run Python tests
make pytest-venv     # Run tests in venv
make pytest-debug    # Run tests with debug build
```

## Package Management

Prefer `uv` over `pip` for virtual environments.

```bash
# Using uv (recommended)
cd tools/python_api
uv venv
source .venv/bin/activate
uv pip install -e .

# Using pip (legacy)
cd tools/python_api
pip install -e .
```

## Build Output

Python extension is built to: `tools/python_api/build/`

## Clean Build

```bash
make clean-python-api
```
