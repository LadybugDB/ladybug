#!/usr/bin/env bash
# Run clang-format (clang18 from Alpine, ~18.1.x) via Docker. Use from repo root.
# Usage:
#   ./scripts/run-clang-format-docker.sh [-i] [paths...]   # check only (exit 1 if not formatted)
#   ./scripts/run-clang-format-docker.sh -i [paths...]     # format in place

set -e
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

IN_PLACE=""
PATHS="-r src/ test/ tools/ extension/"

if [ "$1" = "-i" ]; then
  IN_PLACE="-i"
  shift
  if [ $# -gt 0 ]; then
    PATHS="$@"
  fi
elif [ $# -gt 0 ]; then
  PATHS="$@"
fi

docker run --rm \
  -v "$REPO_ROOT:/src" \
  -w /src \
  alpine:3.20 \
  sh -c "apk add --no-cache -q python3 clang18-extra-tools > /dev/null && python3 scripts/run-clang-format.py --clang-format-executable /usr/lib/llvm18/bin/clang-format $IN_PLACE $PATHS"
