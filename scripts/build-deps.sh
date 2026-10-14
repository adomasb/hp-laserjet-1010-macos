#!/bin/sh
# Local, pinned PAPPL build. No system installation and no downloaded drivers.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
scripts/build-openssl.sh
openssl_prefix=$(python3 tools/dependencies.py openssl path)
version=$(python3 tools/dependencies.py pappl version)
archive="$root/.cache/pappl-$version.tar.gz"
expected=$(python3 tools/dependencies.py pappl sha256)
mkdir -p .cache build/deps
if [ ! -f "$archive" ]; then
  curl --fail --location --proto '=https' --tlsv1.2 "https://github.com/michaelrsweet/pappl/releases/download/v$version/pappl-$version.tar.gz" -o "$archive"
fi
actual=$(shasum -a 256 "$archive" | awk '{print $1}')
[ "$actual" = "$expected" ] || { echo 'PAPPL archive checksum mismatch' >&2; exit 1; }
source="$root/build/deps/pappl-$version"
fingerprint=$(cat scripts/build-deps.sh tools/patch_pappl.py tools/dependencies.py config/dependencies.json src/printer_protocol.h patches/pappl/* "$openssl_prefix/.build-fingerprint" | shasum -a 256 | awk '{print $1}')
if [ -f "$source/pappl/libpappl.a" ] && [ -f "$source/.build-fingerprint" ] &&
   [ "$(cat "$source/.build-fingerprint")" = "$fingerprint" ]; then exit 0; fi
rm -rf "$source"
tar -xzf "$archive" -C "$root/build/deps"
python3 tools/patch_pappl.py "$source"
# Configure requires pkg-config for TLS even when the service disables TLS.
# Narrow local adapter reports only the explicitly supplied OpenSSL library.
mkdir -p "$root/build/deps/bin"
cat > "$root/build/deps/bin/pkg-config" <<SH
#!/bin/sh
case "\$*" in
  '--exists openssl') exit 0 ;;
  '--cflags openssl') echo '-I$openssl_prefix/include' ;;
  '--libs openssl') echo '$openssl_prefix/libssl.a $openssl_prefix/libcrypto.a' ;;
  *) exit 1 ;;
esac
SH
chmod +x "$root/build/deps/bin/pkg-config"
cd "$source"
PATH="$root/build/deps/bin:$PATH" CFLAGS="-O2 -g0 -mmacosx-version-min=26.0 -ffile-prefix-map=$root=." ./configure --prefix="$root/build/deps/local" --disable-shared --disable-libjpeg --disable-libpng --disable-libusb --disable-libpam --with-dnssd=no --with-tls=openssl > "$root/build/deps/configure.log" 2>&1
make -C pappl -j4 libpappl.a > "$root/build/deps/build.log" 2>&1
printf '%s\n' "$fingerprint" > .build-fingerprint
printf '%s\n' "Built pinned PAPPL static library in $source/pappl/libpappl.a"
