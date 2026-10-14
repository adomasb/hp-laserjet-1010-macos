#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
scripts/build-deps.sh
openssl_prefix=$(python3 tools/dependencies.py openssl path)
pappl_prefix=$(python3 tools/dependencies.py pappl path)
clang -std=c11 -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations -arch arm64 -mmacosx-version-min=26.0 \
  -I "$pappl_prefix" -I src tests/test_drain.c src/local_security.c src/fastraster.c \
  "$pappl_prefix/pappl/libpappl.a" \
  "$openssl_prefix/libssl.a" "$openssl_prefix/libcrypto.a" \
  -lcups -lz -lpthread -framework AppKit -framework IOKit -framework CoreFoundation \
  -o build/tests/test-drain
build/tests/test-drain
