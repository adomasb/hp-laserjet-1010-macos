#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
scripts/build-deps.sh
openssl_prefix=$(python3 tools/dependencies.py openssl path)
pappl_prefix=$(python3 tools/dependencies.py pappl path)
clang -std=c11 -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations -arch arm64 -mmacosx-version-min=26.0 \
  -I "$pappl_prefix" -I src tests/test_shutdown.c src/local_security.c src/fastraster.c src/transport.c \
  "$pappl_prefix/pappl/libpappl.a" \
  "$openssl_prefix/libssl.a" "$openssl_prefix/libcrypto.a" \
  -lcups -lz -lpthread -framework AppKit -framework IOKit -framework CoreFoundation \
  -o build/tests/test-shutdown
python3 - <<'PY'
import os, signal, subprocess, time
for mode in ('normal', 'hang'):
    child = subprocess.Popen(['build/tests/test-shutdown', mode], stdout=subprocess.PIPE)
    try:
        assert child.stdout.readline() == b'ready\n'
        started = time.monotonic()
        child.send_signal(signal.SIGTERM)
        code = child.wait(timeout=12)
        elapsed = time.monotonic() - started
        if mode == 'normal':
            assert code == 0 and elapsed < 1.5, (code, elapsed)
        else:
            assert code == 1 and 9.5 <= elapsed < 11.5, (code, elapsed)
        print(f'PASS {mode} shutdown: exit {code}, {elapsed:.2f}s; no USB access')
    finally:
        if child.poll() is None:
            child.kill()
            child.wait()
PY
