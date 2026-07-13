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

dependencies="$(otool -L "$binary")"

for library in libssl.3.dylib libcrypto.3.dylib; do
    if ! grep -Fq "@rpath/$library" <<<"$dependencies"; then
        echo "missing @rpath dependency for $library in $binary" >&2
        exit 1
    fi
done

# The leading '/' is matched by the '^[[:space:]]+/' portion of the regex, so the
# alternatives within the group omit it: 'opt/homebrew', 'usr/local', 'opt/local'.
if grep -Eq '^[[:space:]]+/(opt/homebrew|usr/local|opt/local)/.*lib(ssl|crypto)\.3\.dylib' \
    <<<"$dependencies"; then
    echo "package-manager-specific OpenSSL dependency remains in $binary" >&2
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
