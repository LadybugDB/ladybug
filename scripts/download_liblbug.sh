#!/usr/bin/env bash
# Download Ladybug prebuilt library and header for a given version and platform.
# Unpacks into output_dir/lib/dynamic/{platform}/ and output_dir/include/.
#
# Usage: download_liblbug.sh <version> <platform> [output_dir]
#   version:  release tag (e.g. v0.14.1) or "latest"
#   platform: linux-amd64 | linux-arm64 | darwin | windows-amd64
#   output_dir: default . (current directory)
#
# Example (from a Go binding repo):
#   ./download_liblbug.sh v0.14.1 linux-amd64 .
#   ./download_liblbug.sh latest darwin ./libs
#
# Requires: curl, tar (for .tar.gz), unzip (for Windows .zip).
# Optional: LADYBUG_REPO=Owner/repo to override (default: LadybugDB/ladybug).

set -euo pipefail

REPO="${LADYBUG_REPO:-LadybugDB/ladybug}"
VERSION="${1:?Usage: download_liblbug.sh <version> <platform> [output_dir]}"
PLATFORM="${2:?Usage: download_liblbug.sh <version> <platform> [output_dir]}"
OUT_DIR="${3:-.}"

case "$PLATFORM" in
  linux-amd64)   ASSET="liblbug-linux-x86_64.tar.gz"   ;;
  linux-arm64)   ASSET="liblbug-linux-aarch64.tar.gz"  ;;
  darwin)        ASSET="liblbug-osx-universal.tar.gz"  ;;
  windows-amd64) ASSET="liblbug-windows-x86_64.zip"    ;;
  *)
    echo "Unknown platform: $PLATFORM" >&2
    echo "Supported: linux-amd64, linux-arm64, darwin, windows-amd64" >&2
    exit 1
    ;;
esac

if [ "$VERSION" = "latest" ]; then
  # Resolve latest tag from GitHub API
  VERSION=$(curl -sSf "https://api.github.com/repos/${REPO}/releases/latest" | grep '"tag_name":' | sed -E 's/.*"tag_name":\s*"([^"]+)".*/\1/')
  [ -n "$VERSION" ] || { echo "Could not resolve latest release" >&2; exit 1; }
  echo "Resolved latest: $VERSION"
fi

URL="https://github.com/${REPO}/releases/download/${VERSION}/${ASSET}"
LIB_DIR="${OUT_DIR}/lib/dynamic/${PLATFORM}"
INC_DIR="${OUT_DIR}/include"
mkdir -p "$LIB_DIR" "$INC_DIR"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "Downloading $URL ..."
curl -sSfL -o "$TMP/asset" "$URL"

case "$ASSET" in
  *.tar.gz)
    tar -xzf "$TMP/asset" -C "$TMP"
    # Tarball has lbug.h, lbug.hpp, liblbug.so or liblbug.dylib at root
    [ -f "$TMP/lbug.h" ] && cp "$TMP/lbug.h" "$INC_DIR/"
    [ -f "$TMP/liblbug.so" ] && cp "$TMP/liblbug.so" "$LIB_DIR/"
    [ -f "$TMP/liblbug.dylib" ] && cp "$TMP/liblbug.dylib" "$LIB_DIR/"
    ;;
  *.zip)
    unzip -q -o "$TMP/asset" -d "$TMP"
    [ -f "$TMP/lbug.h" ] && cp "$TMP/lbug.h" "$INC_DIR/"
    [ -f "$TMP/lbug_shared.dll" ] && cp "$TMP/lbug_shared.dll" "$LIB_DIR/"
    [ -f "$TMP/lbug_shared.lib" ] && cp "$TMP/lbug_shared.lib" "$LIB_DIR/"
    ;;
esac

echo "Done. Library in $LIB_DIR, header in $INC_DIR"
