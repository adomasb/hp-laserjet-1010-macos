#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests .cache/swift-modules
swiftc -swift-version 5 -module-cache-path "$PWD/.cache/swift-modules" \
  -import-objc-header src/printer_protocol.h src/PrinterSetup.swift src/PrintingService.swift \
  tests/test_printing_service.swift -o build/tests/test-printing-service
build/tests/test-printing-service
