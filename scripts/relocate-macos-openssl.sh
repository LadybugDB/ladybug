#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <mach-o-binary>" >&2
    exit 2
fi

binary="$1"
if [ ! -f "$binary" ]; then
    echo "Mach-O binary not found: $binary" >&2
    exit 1
fi

dependency_for() {
    local library="$1"
    otool -L "$binary" | awk -v library="$library" \
        'index($1, library) && substr($1, length($1) - length(library) + 1) == library { print $1; exit }'
}

for library in libssl.3.dylib libcrypto.3.dylib; do
    dependency="$(dependency_for "$library")"
    if [ -z "$dependency" ]; then
        echo "OpenSSL dependency $library not found in $binary" >&2
        exit 1
    fi
    if [ "$dependency" != "@rpath/$library" ]; then
        install_name_tool -change "$dependency" "@rpath/$library" "$binary"
    fi
done

# Drop build-environment rpaths (CI pixi/conda envs) so the shipped binary
# does not carry the runner's absolute path (e.g.
# /Users/runner/work/ladybug/ladybug/.pixi/envs/default/lib). dyld skips
# nonexistent rpath dirs, but leaking the CI path breaks `LOAD` on user
# machines where the OpenSSL lookup falls through to it. See #1069.
existing_rpaths="$(otool -l "$binary" | awk '/cmd LC_RPATH/{getline; getline; print $2}')"
while IFS= read -r rpath; do
    [ -n "$rpath" ] || continue
    case "$rpath" in
        *.pixi/*|*conda*|*miniconda*|*/runner/work/*)
            install_name_tool -delete_rpath "$rpath" "$binary"
            ;;
    esac
done <<<"$existing_rpaths"

for rpath in \
    /opt/homebrew/opt/openssl@3/lib \
    /usr/local/opt/openssl@3/lib \
    /opt/local/lib; do
    existing_rpaths="$(otool -l "$binary" | awk '/cmd LC_RPATH/{getline; getline; print $2}')"
    if ! grep -Fxq "$rpath" <<<"$existing_rpaths"; then
        install_name_tool -add_rpath "$rpath" "$binary"
    fi
done

# install_name_tool invalidates the existing signature.
# Ad-hoc signing ("-") is sufficient for development and CI. If this binary is
# distributed to end-users (e.g., via npm), a proper Developer ID certificate
# should be used instead for notarization compatibility.
codesign --force --sign - "$binary"
