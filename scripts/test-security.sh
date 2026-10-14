#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
clang -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -fsanitize=address,undefined -I src \
  tests/test_local_security.c -lcups -o build/tests/test-local-security
build/tests/test-local-security
clang -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -I src \
  tests/test_auth_client.c src/local_security.c -lcups -o build/tests/test-auth-client
python3 tests/security.py
