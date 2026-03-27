#!/usr/bin/env bash
# Download prebuilt liblbug archives from GitHub releases.
set -euo pipefail

LIB_KIND="${LBUG_LIB_KIND:-shared}"
LINUX_VARIANT="${LBUG_LINUX_VARIANT:-compat}"

if [ "$LIB_KIND" != "shared" ] && [ "$LIB_KIND" != "static" ]; then
  echo "Unsupported LBUG_LIB_KIND: $LIB_KIND (expected 'shared' or 'static')" >&2
  exit 1
fi

if [ "$LINUX_VARIANT" != "compat" ] && [ "$LINUX_VARIANT" != "perf" ]; then
  echo "Unsupported LBUG_LINUX_VARIANT: $LINUX_VARIANT (expected 'compat' or 'perf')" >&2
  exit 1
fi

LBUG_VERSION=$(curl -sS https://api.github.com/repos/LadybugDB/ladybug/releases/latest | grep -o '"tag_name": "v\([^"]*\)"' | cut -d'"' -f4 | cut -c2-)
RELEASE_URL="https://github.com/LadybugDB/ladybug/releases/download/v${LBUG_VERSION}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TARGET_DIR="${LBUG_TARGET_DIR:-$PROJECT_DIR/lib}"

OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
  Darwin)
    if [ "$ARCH" = "x86_64" ]; then
      MACOS_ARCHIVE_ARCH="x86_64"
    elif [ "$ARCH" = "arm64" ]; then
      MACOS_ARCHIVE_ARCH="arm64"
    else
      echo "Unsupported macOS architecture: $ARCH" >&2
      exit 1
    fi
    if [ "$LIB_KIND" = "static" ]; then
      ARCHIVE="liblbug-static-osx-${MACOS_ARCHIVE_ARCH}.tar.gz"
      LIB_NAME="liblbug.a"
    else
      ARCHIVE="liblbug-osx-${MACOS_ARCHIVE_ARCH}.tar.gz"
      LIB_NAME="liblbug.dylib"
    fi
    ;;
  Linux)
    if [ "$ARCH" = "x86_64" ]; then
      LINUX_ARCHIVE_ARCH="x86_64"
    elif [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
      LINUX_ARCHIVE_ARCH="aarch64"
    else
      echo "Unsupported Linux architecture: $ARCH" >&2
      exit 1
    fi
    if [ "$LIB_KIND" = "static" ]; then
      ARCHIVE="liblbug-static-linux-${LINUX_ARCHIVE_ARCH}-${LINUX_VARIANT}.tar.gz"
      LIB_NAME="liblbug.a"
    else
      ARCHIVE="liblbug-linux-${LINUX_ARCHIVE_ARCH}.tar.gz"
      LIB_NAME="liblbug.so"
    fi
    ;;
  MINGW*|MSYS*|CYGWIN*)
    if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "AMD64" ]; then
      WINDOWS_ARCHIVE_ARCH="x86_64"
    else
      echo "Unsupported Windows architecture: $ARCH" >&2
      exit 1
    fi
    if [ "$LIB_KIND" = "static" ]; then
      ARCHIVE="liblbug-static-windows-${WINDOWS_ARCHIVE_ARCH}.zip"
      LIB_NAME="lbug.lib"
    else
      ARCHIVE="liblbug-windows-${WINDOWS_ARCHIVE_ARCH}.zip"
      LIB_NAME="lbug_shared.dll"
    fi
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
