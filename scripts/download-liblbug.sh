#!/usr/bin/env bash
# Download prebuilt liblbug archives from GitHub releases or workflow artifacts.
set -euo pipefail

LIB_KIND="${LBUG_LIB_KIND:-shared}"
LINUX_VARIANT="${LBUG_LINUX_VARIANT:-compat}"
REPOSITORY="${LBUG_GITHUB_REPOSITORY:-LadybugDB/ladybug}"
RUN_ID="${LBUG_PRECOMPILED_RUN_ID:-}"
VERSION_OVERRIDE="${LBUG_VERSION:-}"

if [ "$LIB_KIND" != "shared" ] && [ "$LIB_KIND" != "static" ]; then
  echo "Unsupported LBUG_LIB_KIND: $LIB_KIND (expected 'shared' or 'static')" >&2
  exit 1
fi

if [ "$LINUX_VARIANT" != "compat" ] && [ "$LINUX_VARIANT" != "perf" ]; then
  echo "Unsupported LBUG_LINUX_VARIANT: $LINUX_VARIANT (expected 'compat' or 'perf')" >&2
  exit 1
fi

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
      ARTIFACT_NAME="liblbug-static-osx-${MACOS_ARCHIVE_ARCH}"
      LIB_NAME="liblbug.a"
    else
      ARCHIVE="liblbug-osx-${MACOS_ARCHIVE_ARCH}.tar.gz"
      ARTIFACT_NAME="liblbug-osx-${MACOS_ARCHIVE_ARCH}"
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
      ARTIFACT_NAME="liblbug-static-linux-${LINUX_ARCHIVE_ARCH}-${LINUX_VARIANT}"
      LIB_NAME="liblbug.a"
    else
      ARCHIVE="liblbug-linux-${LINUX_ARCHIVE_ARCH}.tar.gz"
      ARTIFACT_NAME="liblbug-linux-${LINUX_ARCHIVE_ARCH}"
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
      ARTIFACT_NAME="liblbug-static-windows-${WINDOWS_ARCHIVE_ARCH}"
      LIB_NAME="lbug.lib"
    else
      ARCHIVE="liblbug-windows-${WINDOWS_ARCHIVE_ARCH}.zip"
      ARTIFACT_NAME="liblbug-windows-${WINDOWS_ARCHIVE_ARCH}"
      LIB_NAME="lbug_shared.dll"
    fi
    ;;
  *)
    echo "Unsupported OS: $OS" >&2
    exit 1
    ;;
esac

if [ -f "$TARGET_DIR/$LIB_NAME" ]; then
  echo "liblbug already exists in $TARGET_DIR"
  exit 0
fi

mkdir -p "$TARGET_DIR"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

fetch_release_archive() {
  local version
  if [ -n "$VERSION_OVERRIDE" ]; then
    version="$VERSION_OVERRIDE"
  else
    version="$(curl -sS "https://api.github.com/repos/${REPOSITORY}/releases/latest" | grep -o '"tag_name": "v\([^"]*\)"' | cut -d'"' -f4 | cut -c2-)"
  fi
  local download_url="https://github.com/${REPOSITORY}/releases/download/v${version}/${ARCHIVE}"
  curl -fSL "$download_url" -o "$TMPDIR/$ARCHIVE"
  echo "release:v${version}"
}

fetch_run_artifact() {
  if ! command -v gh >/dev/null 2>&1; then
    echo "gh CLI is required when LBUG_PRECOMPILED_RUN_ID is set" >&2
    exit 1
  fi
  gh run download "$RUN_ID" --repo "$REPOSITORY" --name "$ARTIFACT_NAME" --dir "$TMPDIR/artifact" >/dev/null
  local extracted_archive
  extracted_archive="$(find "$TMPDIR/artifact" -type f -name "$ARCHIVE" | head -n1)"
  if [ -z "$extracted_archive" ]; then
    echo "Artifact ${ARTIFACT_NAME} does not contain ${ARCHIVE}" >&2
    exit 1
  fi
  mv "$extracted_archive" "$TMPDIR/$ARCHIVE"
  echo "run:${RUN_ID}/${ARTIFACT_NAME}"
}

if [ -n "$RUN_ID" ]; then
  SOURCE_DESC="$(fetch_run_artifact)"
else
  SOURCE_DESC="$(fetch_release_archive)"
fi

if [[ "$ARCHIVE" == *.zip ]]; then
  unzip -o "$TMPDIR/$ARCHIVE" -d "$TARGET_DIR"
else
  tar xzf "$TMPDIR/$ARCHIVE" -C "$TARGET_DIR"
fi

echo "Installed ${ARCHIVE} from ${SOURCE_DESC} to $TARGET_DIR"
