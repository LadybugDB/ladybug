#!/bin/bash
# Amalgamate lz4 source files for third_party

set -e

# Run from srcdir (third_party/work/lz4/lz4-1.10.0)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC_DIR="$TOP_DIR/work/lz4/lz4-1.10.0/lib"
DEST_DIR="$TOP_DIR/lz4"

echo "Amalgamating lz4..."
echo "  SRC_DIR: $SRC_DIR"
echo "  DEST_DIR: $DEST_DIR"

# Create header: just lz4.h - NO namespace wrapper
cp "$SRC_DIR/lz4.h" "$DEST_DIR/lz4.hpp"

# Fix include for header (in case it self-references)
sed -i '' 's|#include "lz4\.h"|#include "lz4.hpp"|g' "$DEST_DIR/lz4.hpp"

# Create source: just lz4.c
cp "$SRC_DIR/lz4.c" "$DEST_DIR/lz4.cpp"

# Fix include for source
sed -i '' 's|#include "lz4\.h"|#include "lz4.hpp"|g' "$DEST_DIR/lz4.cpp"

# Copy LICENSE
cp "$SRC_DIR/LICENSE" "$DEST_DIR/LICENSE"

echo "Done. Files created:"
ls -la "$DEST_DIR"
