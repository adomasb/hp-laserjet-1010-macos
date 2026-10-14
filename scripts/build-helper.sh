#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
scripts/build-deps.sh
mkdir -p build
openssl_prefix=$(python3 tools/dependencies.py openssl path)
pappl_prefix=$(python3 tools/dependencies.py pappl path)
cc -std=c11 -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations -arch arm64 -mmacosx-version-min=26.0 \
  "-ffile-prefix-map=$root=." -Wl,-S -I "$pappl_prefix" -I src \
  src/service.c src/local_security.c src/fastraster.c src/transport.c \
  "$pappl_prefix/pappl/libpappl.a" \
  "$openssl_prefix/libssl.a" "$openssl_prefix/libcrypto.a" \
  -lcups -lz -lpthread -framework AppKit -framework IOKit -framework CoreFoundation \
  -o build/hp1010-helper
strip -S build/hp1010-helper
printf '%s\n' 'Built build/hp1010-helper (Apple Silicon)'
