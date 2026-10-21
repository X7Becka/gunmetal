#!/bin/sh
# Fetch the stock Ubuntu ASan/UBSan shared runtimes into ~/tools/sanitizers.
# No sudo, no apt-get install: just download the .deb and unpack it with
# dpkg-deb -x into a user-writable prefix.
set -eu

PREFIX="${1:-$HOME/tools/sanitizers}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$PREFIX"
cd "$WORK"

echo "==> downloading libasan8 and libubsan1"
apt-get download libasan8 libubsan1

for deb in ./*.deb; do
    echo "==> unpacking $deb -> $PREFIX"
    dpkg-deb -x "$deb" "$PREFIX"
done

LIB="$PREFIX/usr/lib/x86_64-linux-gnu"
echo "==> done"
ls -l "$LIB"/libasan.so.8* "$LIB"/libubsan.so.1*
echo
echo "Now: cmake --preset asan"
