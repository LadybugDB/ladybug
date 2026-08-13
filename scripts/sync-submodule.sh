#!/usr/bin/env bash
# sync-submodule.sh — reset every submodule to the tip of its upstream default
# branch (origin/main by default) and record the new pointers in a single
# "Update submodules" commit on the superproject.
#
# NOTE: this discards local commits and tracked-file modifications inside each
# submodule (git checkout -f). Untracked files are left alone unless --clean
# is passed.
#
# Usage:
#   scripts/sync-submodule.sh [--clean]
#
# Options:
#   --clean   also delete untracked files inside each submodule (git clean -fdx)
#
# Environment:
#   SYNC_SUBMODULE_BRANCH   remote branch to sync to (default: main)
set -euo pipefail

BRANCH="${SYNC_SUBMODULE_BRANCH:-main}"
CLEAN=0

for arg in "$@"; do
  case "$arg" in
    --clean) CLEAN=1 ;;
    -h|--help)
      sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "error: unknown option: $arg" >&2
      exit 1
      ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "error: not inside a git working tree ($PROJECT_DIR)" >&2
  exit 1
fi

if ! git config --file .gitmodules --get-regexp '^submodule\..*\.path$' >/dev/null 2>&1; then
  echo "No submodules configured; nothing to do."
  exit 0
fi

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "warning: superproject has uncommitted changes; only submodule pointers will be committed" >&2
fi

echo "==> Syncing submodule URLs (git submodule sync)"
git submodule sync

echo "==> Initializing submodules (git submodule update --init)"
git submodule update --init

echo "==> Resetting submodules to origin/$BRANCH"
export SYNC_BRANCH="$BRANCH"
export SYNC_CLEAN="$CLEAN"
git submodule foreach '
  if [ "$SYNC_CLEAN" = "1" ]; then
    git clean -fdx >/dev/null
  fi

  git fetch --prune origin

  branch="$SYNC_BRANCH"
  if ! git show-ref --verify --quiet "refs/remotes/origin/$branch"; then
    default="$(git symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null || true)"
    if [ -n "$default" ]; then
      branch="${default#origin/}"
      echo "    [$name] origin/$SYNC_BRANCH not found; falling back to origin/$branch" >&2
    else
      echo "error: [$name] cannot resolve origin/$SYNC_BRANCH or origin/HEAD" >&2
      exit 1
    fi
  fi

  echo "    [$name] -> origin/$branch ($(git rev-parse --short "origin/$branch"))"
  git checkout -f -B "$branch" "origin/$branch"
'

echo "==> Staging submodule pointer updates"
git submodule foreach --quiet 'echo "$sm_path"' | while IFS= read -r path; do
  git add "$path"
done

if git diff --cached --quiet; then
  echo "No submodule pointer changes; nothing to commit."
else
  git commit -m "Update submodules"
  echo "Committed: $(git rev-parse --short HEAD)"
fi
