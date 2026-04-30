#!/usr/bin/bash

set -euo pipefail

CD=$(dirname "$0")
DATASET_DIR=$CD/../dataset
DEMO_DATASET_DIR=$DATASET_DIR/demo-db/parquet

SERIALIZER_DATASET_DIR=$DEMO_DATASET_DIR
TEMP_DATASET_DIR=
cleanup() {
    if [ -n "$TEMP_DATASET_DIR" ]; then
        rm -rf "$TEMP_DATASET_DIR"
    fi
}
trap cleanup EXIT

if [ "${OS:-}" = "Windows_NT" ]; then
    TEMP_DATASET_DIR=$(mktemp -d)
    cp "$DEMO_DATASET_DIR/schema.cypher" "$TEMP_DATASET_DIR/schema.cypher"
    if command -v cygpath >/dev/null 2>&1; then
        DEMO_DATASET_PATH=$(cygpath -am "$DEMO_DATASET_DIR")
    else
        DEMO_DATASET_PATH=$(python3 - "$DEMO_DATASET_DIR" <<'PY'
from pathlib import Path
import sys

print(Path(sys.argv[1]).resolve().as_posix())
PY
)
    fi
    python3 - "$DEMO_DATASET_PATH" "$DEMO_DATASET_DIR/copy.cypher" "$TEMP_DATASET_DIR/copy.cypher" <<'PY'
import os
import re
import sys

base_path, input_path, output_path = sys.argv[1:]

with open(input_path, encoding="utf-8") as input_file:
    copy_lines = input_file.readlines()

def replace_path(match):
    path = match.group(1)
    if os.path.isabs(path) or re.match(r"^[A-Za-z]:[\\/]", path):
        return f'"{path.replace(chr(92), "/")}"'
    return f'"{base_path.rstrip("/")}/{path.replace(chr(92), "/")}"'

with open(output_path, "w", encoding="utf-8") as output_file:
    for line in copy_lines:
        output_file.write(re.sub(r'"([^"]*)"', replace_path, line))
PY
    SERIALIZER_DATASET_DIR=$TEMP_DATASET_DIR
fi

python3 $CD/../benchmark/serializer.py DemoDB "$SERIALIZER_DATASET_DIR" $DATASET_DIR/binary-demo --single-thread "$@"
