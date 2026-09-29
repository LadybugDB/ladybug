#!/usr/bin/env bash
set -euo pipefail

# Verifies that a macOS Mach-O binary's OpenSSL dependency has been relocated
# by scripts/relocate-macos-openssl.sh: it must reference
# @rpath/libssl.3.dylib and @rpath/libcrypto.3.dylib, must not reference any
# build-environment absolute OpenSSL path (pixi/conda, CI runner work dir),
# and must carry the common Homebrew/MacPorts OpenSSL locations as LC_RPATH.
# See https://github.com/LadybugDB/ladybug/issues/1069.

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <mach-o-binary>" >&2
    exit 2
fi

binary="$1"
if [ ! -f "$binary" ]; then
    echo "Mach-O binary not found: $binary" >&2
    exit 1
fi

dependencies="$(otool -L "$binary")"

for library in libssl.3.dylib libcrypto.3.dylib; do
    if ! grep -Fq "@rpath/$library" <<<"$dependencies"; then
        echo "missing @rpath dependency for $library in $binary" >&2
        exit 1
    fi
done

# No absolute OpenSSL dependency may remain (Homebrew, MacPorts, pixi/conda,
# or any other absolute path ending in libssl/libcrypto).
if grep -Eq '^[[:space:]]+/.*lib(ssl|crypto)(\.3)?\.dylib' <<<"$dependencies"; then
    echo "absolute OpenSSL dependency remains in $binary" >&2
    exit 1
fi

rpaths="$(otool -l "$binary" | awk '/cmd LC_RPATH/{getline; getline; print $2}')"
for required in \
    /opt/homebrew/opt/openssl@3/lib \
    /usr/local/opt/openssl@3/lib \
    /opt/local/lib; do
    if ! grep -Fxq "$required" <<<"$rpaths"; then
        echo "missing OpenSSL rpath $required in $binary" >&2
        exit 1
    fi
done

# Build-environment rpaths (CI pixi/conda envs) must not ship.
while IFS= read -r rpath; do
    [ -n "$rpath" ] || continue
    case "$rpath" in
        *.pixi/*|*conda*|*miniconda*|*/runner/work/*)
            echo "build-environment rpath $rpath remains in $binary" >&2
            exit 1
            ;;
    esac
done <<<"$rpaths"
