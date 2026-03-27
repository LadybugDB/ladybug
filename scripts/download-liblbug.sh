#!/usr/bin/env bash
# Download prebuilt liblbug shared library from GitHub releases.
set -euo pipefail

LBUG_VERSION=$(curl -sS https://api.github.com/repos/LadybugDB/ladybug/releases/latest | grep -o '"tag_name": "v\([^"]*\)"' | cut -d'"' -f4 | cut -c2-)
RELEASE_URL="https://github.com/LadybugDB/ladybug/releases/download/v${LBUG_VERSION}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TARGET_DIR="$PROJECT_DIR/lib"

OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
  Darwin)
    ARCHIVE="liblbug-osx-universal.tar.gz"
    LIB_NAME="liblbug.dylib"
    ;;
  Linux)
    if [ "$ARCH" = "x86_64" ]; then
      ARCHIVE="liblbug-linux-x86_64.tar.gz"
    elif [ "$ARCH" = "aarch64" ]; then
      ARCHIVE="liblbug-linux-aarch64.tar.gz"
    else
      echo "Unsupported Linux architecture: $ARCH" >&2
      exit 1
    fi
    LIB_NAME="liblbug.so"
    ;;
  MINGW*|MSYS*|CYGWIN*)
    if [ "$ARCH" = "x86_64" ]; then
      ARCHIVE="liblbug-windows-x86_64.zip"
    elif [ "$ARCH" = "aarch64" ]; then
      ARCHIVE="liblbug-windows-aarch64.zip"
    else
      echo "Unsupported Windows architecture: $ARCH" >&2
      exit 1
    fi
    LIB_NAME="liblbug.dll"
    ;;
  *)
    echo "Unsupported OS: $OS" >&2
    exit 1
    ;;
esac

DOWNLOAD_URL="${RELEASE_URL}/${ARCHIVE}"

if [ -f "$TARGET_DIR/$LIB_NAME" ]; then
  echo "liblbug already exists in $TARGET_DIR"
  exit 0
fi

mkdir -p "$TARGET_DIR"
TMPFILE="$(mktemp)"
trap "rm -f '$TMPFILE'" EXIT

curl -fSL "$DOWNLOAD_URL" -o "$TMPFILE"

if [[ "$ARCHIVE" == *.zip ]]; then
  unzip -o "$TMPFILE" -d "$TARGET_DIR"
else
  tar xzf "$TMPFILE" -C "$TARGET_DIR"
fi

echo "Installed liblbug v${LBUG_VERSION} to $TARGET_DIR"
