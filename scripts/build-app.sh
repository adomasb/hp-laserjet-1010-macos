#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
version=$(python3 tools/project_version.py)
scripts/build-helper.sh
pappl_prefix=$(python3 tools/dependencies.py pappl path)
openssl_prefix=$(python3 tools/dependencies.py openssl path)
stage=$(mktemp -d "${TMPDIR:-/tmp}/hp1010-app.XXXXXX")
trap 'rm -rf "$stage"' EXIT
app="$stage/LaserJet 1010 Helper.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Helpers" "$app/Contents/Resources/Licences"
swiftc -swift-version 5 -O -target arm64-apple-macosx26.0 -module-cache-path "$PWD/.cache/swift-modules" -import-objc-header src/printer_protocol.h src/App.swift src/AppInstallation.swift src/PrinterSetup.swift src/PrintingService.swift -o "$app/Contents/MacOS/LaserJet1010Helper" -framework AppKit -framework ServiceManagement
swift -module-cache-path "$PWD/.cache/swift-modules" tools/make-app-icon.swift "$stage/AppIcon.iconset"
iconutil -c icns "$stage/AppIcon.iconset" -o "$app/Contents/Resources/AppIcon.icns"
cp build/hp1010-helper "$app/Contents/Helpers/"
strip -S "$app/Contents/MacOS/LaserJet1010Helper" "$app/Contents/Helpers/hp1010-helper"
cp THIRD_PARTY_NOTICES.md "$app/Contents/Resources/"
cp LICENSE "$app/Contents/Resources/Licences/HP1010-MIT-LICENSE.txt"
cp "$pappl_prefix/LICENSE" "$pappl_prefix/NOTICE" "$app/Contents/Resources/Licences/"
if [[ -d licences ]]; then cp licences/* "$app/Contents/Resources/Licences/"; fi
cp "$openssl_prefix/LICENSE.txt" "$app/Contents/Resources/Licences/OpenSSL-LICENSE.txt"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>dev.local.hp1010.helper</string>
<key>CFBundleName</key><string>HP LaserJet 1010</string>
<key>CFBundleDisplayName</key><string>HP LaserJet 1010</string>
<key>CFBundleIconFile</key><string>AppIcon.icns</string>
<key>CFBundleExecutable</key><string>LaserJet1010Helper</string>
<key>CFBundleVersion</key><string>$version</string>
<key>CFBundleShortVersionString</key><string>$version</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>LSMinimumSystemVersion</key><string>26.0</string>
<key>LSUIElement</key><true/>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
codesign --force --sign - "$app/Contents/Helpers/hp1010-helper"
xattr -d com.apple.FinderInfo "$app" 2>/dev/null || true
codesign --force --sign - "$app"
codesign --verify --deep --strict "$app"
python3 tools/check-release-privacy.py "$app"
destination="$PWD/build/LaserJet 1010 Helper.app"
rm -rf "$destination"
ditto --norsrc --noextattr "$app" "$destination"
xattr -d com.apple.FinderInfo "$destination" 2>/dev/null || true
printf 'Built %s (version %s)\n' "$destination" "$version"
