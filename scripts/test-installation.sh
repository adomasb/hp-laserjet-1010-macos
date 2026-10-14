#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests .cache/swift-modules
swiftc -swift-version 5 -module-cache-path "$PWD/.cache/swift-modules" \
  src/AppInstallation.swift tests/test_installation.swift -framework AppKit \
  -o build/tests/test-installation
build/tests/test-installation
