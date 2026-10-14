#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests .cache
flags=(-std=c11 -Wall -Wextra -Werror -g -I src)
if [[ ${SANITIZE:-0} == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
clang "${flags[@]}" src/fastraster.c tests/test_fastraster.c -o build/tests/test-fastraster
build/tests/test-fastraster .cache/core-pattern.fr
clang "${flags[@]}" src/transport.c tests/test_transport.c -framework IOKit -framework CoreFoundation -o build/tests/test-transport
build/tests/test-transport
python3 tools/decode_fastraster.py --self-test
python3 tools/decode_fastraster.py .cache/core-pattern.fr > .cache/core-pattern-summary.json
