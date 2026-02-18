#!/usr/bin/env bash
# Run clang-format (18.1.3, same as CI) via Docker. Use from repo root.
# Usage:
#   ./scripts/run-clang-format-docker.sh          # check only (exit 1 if not formatted)
#   ./scripts/run-clang-format-docker.sh -i       # format in place

set -e
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

IN_PLACE=""
for arg in "$@"; do
  if [ "$arg" = "-i" ] || [ "$arg" = "--in-place" ]; then
    IN_PLACE="-i"
    break
  fi
done

docker run --rm \
  -v "$REPO_ROOT:/src" \
  -w /src \
  ubuntu:24.04 \
  bash -c "apt-get update -qq && apt-get install -y -qq clang-format-18 > /dev/null && python3 scripts/run-clang-format.py --clang-format-executable /usr/bin/clang-format-18 -r src/ test/ tools/ extension/ $IN_PLACE"
