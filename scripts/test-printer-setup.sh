#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
stage=$(mktemp -d "${TMPDIR:-/tmp}/hp1010-printer-test.XXXXXX")
trap 'rm -rf "$stage"' EXIT
swiftc -swift-version 5 -module-cache-path "$stage/modules" -import-objc-header src/printer_protocol.h src/PrinterSetup.swift tests/test_printer_setup.swift -o "$stage/test-printer-setup"
"$stage/test-printer-setup"
