#!/bin/bash
# Complete software validation; no physical printing or system queue changes.
set -euo pipefail
cd "$(dirname "$0")/.."
scripts/build-helper.sh
python3 tests/test_dependency_patches.py
SANITIZE=1 HP1010_SKIP_USB_PROBE=1 scripts/test-core.sh
scripts/test-installation.sh
scripts/test-printer-setup.sh
scripts/test-printing-service.sh
scripts/test-security.sh
python3 tests/integration.py
python3 tests/regression.py
scripts/test-shutdown.sh
scripts/test-drain.sh
python3 tests/multipart.py
python3 tests/test_release_privacy.py
python3 tests/test_release.py
python3 tools/check-release-privacy.py
scripts/release.sh --package-only
