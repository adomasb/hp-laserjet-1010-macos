#!/bin/sh
# Build a verified dependency locally; never change the system installation.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
version=$(python3 tools/dependencies.py openssl version)
expected=$(python3 tools/dependencies.py openssl sha256)
archive="$root/.cache/openssl-$version.tar.gz"
source="$root/build/deps/openssl-$version"
mkdir -p .cache build/deps
if [ ! -f "$archive" ]; then
  download=$(mktemp "$archive.partial.XXXXXX")
  trap 'rm -f "$download"' EXIT
  curl --fail --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
    "https://github.com/openssl/openssl/releases/download/openssl-$version/openssl-$version.tar.gz" -o "$download"
  [ "$(shasum -a 256 "$download" | awk '{print $1}')" = "$expected" ] || { echo 'OpenSSL checksum mismatch' >&2; exit 1; }
  mv "$download" "$archive"
  trap - EXIT
fi
[ "$(shasum -a 256 "$archive" | awk '{print $1}')" = "$expected" ] || { echo 'OpenSSL checksum mismatch' >&2; exit 1; }
fingerprint=$(cat scripts/build-openssl.sh tools/dependencies.py config/dependencies.json | shasum -a 256 | awk '{print $1}')
if [ -f "$source/libssl.a" ] && [ -f "$source/libcrypto.a" ] &&
   [ -f "$source/.build-fingerprint" ] && [ "$(cat "$source/.build-fingerprint")" = "$fingerprint" ]; then exit 0; fi
rm -rf "$source"
tar -xzf "$archive" -C "$root/build/deps"
cd "$source"
./Configure darwin64-arm64-cc no-shared no-tests no-module \
  --prefix=/usr/local --openssldir=/etc/ssl \
  -O2 -g0 -mmacosx-version-min=26.0 \
  > "$root/build/deps/openssl-configure.log" 2>&1
make -j4 build_libs > "$root/build/deps/openssl-build.log" 2>&1
printf '%s\n' "$fingerprint" > .build-fingerprint
printf 'Built pinned OpenSSL %s\n' "$version"
