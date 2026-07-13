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
codesign --force --sign - "$binary"
