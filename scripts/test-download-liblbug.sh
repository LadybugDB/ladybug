#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT

bin_dir="$test_dir/bin"
mkdir -p "$bin_dir"

cat > "$bin_dir/uname" <<'EOF'
#!/usr/bin/env bash
case "${1:-}" in
  -s) printf 'Linux\n' ;;
  -m) printf 'x86_64\n' ;;
  *) exit 1 ;;
esac
EOF

cat > "$bin_dir/gh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

printf '%s\n' "$@" > "$TEST_GH_ARGS"
artifact_dir=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --dir)
      artifact_dir="$2"
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done

mkdir -p "$artifact_dir" "$TEST_PAYLOAD_DIR"
printf 'fixture\n' > "$TEST_PAYLOAD_DIR/liblbug.a"
tar czf "$artifact_dir/liblbug-static-linux-x86_64-compat.tar.gz" \
  -C "$TEST_PAYLOAD_DIR" liblbug.a
EOF

cat > "$bin_dir/curl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

printf '%s\n' "$@" > "$TEST_CURL_ARGS"
output=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o)
      output="$2"
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done

mkdir -p "$(dirname "$output")" "$TEST_PAYLOAD_DIR"
printf 'fixture\n' > "$TEST_PAYLOAD_DIR/liblbug.a"
tar czf "$output" -C "$TEST_PAYLOAD_DIR" liblbug.a
EOF

chmod +x "$bin_dir/uname" "$bin_dir/gh" "$bin_dir/curl"

export PATH="$bin_dir:$PATH"
export TEST_GH_ARGS="$test_dir/gh-args"
export TEST_CURL_ARGS="$test_dir/curl-args"
export TEST_PAYLOAD_DIR="$test_dir/payload"
export GITHUB_REPOSITORY="fork-owner/ladybug"
export LBUG_LIB_KIND="static"
export LBUG_PRECOMPILED_RUN_ID="12345"

export LBUG_TARGET_DIR="$test_dir/current-repository"
"$root_dir/scripts/download-liblbug.sh"
test -s "$LBUG_TARGET_DIR/liblbug.a"
test "$(awk '$0 == "--repo" { getline; print; exit }' "$TEST_GH_ARGS")" = \
  "fork-owner/ladybug"

export LBUG_GITHUB_REPOSITORY="override-owner/ladybug"
export LBUG_TARGET_DIR="$test_dir/explicit-override"
"$root_dir/scripts/download-liblbug.sh"
test -s "$LBUG_TARGET_DIR/liblbug.a"
test "$(awk '$0 == "--repo" { getline; print; exit }' "$TEST_GH_ARGS")" = \
  "override-owner/ladybug"

unset LBUG_GITHUB_REPOSITORY LBUG_PRECOMPILED_RUN_ID
export LBUG_VERSION="0.18.1"
export LBUG_TARGET_DIR="$test_dir/release"
"$root_dir/scripts/download-liblbug.sh"
test -s "$LBUG_TARGET_DIR/liblbug.a"
grep -Fx \
  "https://github.com/LadybugDB/ladybug/releases/download/v0.18.1/liblbug-static-linux-x86_64-compat.tar.gz" \
  "$TEST_CURL_ARGS"
