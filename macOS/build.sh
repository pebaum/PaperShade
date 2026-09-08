#!/bin/bash
set -euo pipefail

usage() {
    printf 'Usage: bash macOS/build.sh [--arch arm64|x86_64|universal]\n'
}

ARCH=universal
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)
            if [[ $# -lt 2 ]]; then usage >&2; exit 2; fi
            ARCH="$2"
            shift 2
            ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
case "$ARCH" in
    arm64|x86_64|universal) ;;
    *) printf 'Unsupported architecture: %s\n' "$ARCH" >&2; exit 2 ;;
esac
if [[ "$(uname -s)" != Darwin ]]; then
    printf 'This script requires macOS with Xcode or its command-line developer tools.\n' >&2
    exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(tr -d '\r\n' < "$ROOT/VERSION")"
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    printf 'VERSION must contain a three-component numeric version, got: %s\n' "$VERSION" >&2
    exit 1
fi
SWIFT="$(xcrun --find swift)"
export MACOSX_DEPLOYMENT_TARGET=13.0
export CLANG_MODULE_CACHE_PATH="$ROOT/.build/macos-module-cache"
APP="$ROOT/dist/PaperShade.app"
ZIP="$ROOT/dist/PaperShade-$VERSION-macos-$ARCH.zip"
DMG="$ROOT/dist/PaperShade-$VERSION-macos-$ARCH.dmg"
ICONSET="$ROOT/.build/macos-packaging/PaperShade.iconset"

build_arch() {
    local arch="$1"
    "$SWIFT" build --package-path "$ROOT" --scratch-path "$ROOT/.build/macos-$arch" \
        --configuration release --arch "$arch" --product PaperShade
}

bin_path() {
    local arch="$1"
    "$SWIFT" build --package-path "$ROOT" --scratch-path "$ROOT/.build/macos-$arch" \
        --configuration release --arch "$arch" --show-bin-path
}

if [[ "$ARCH" == universal ]]; then
    build_arch arm64
    build_arch x86_64
    ARM_BINARY="$(bin_path arm64)/PaperShade"
    INTEL_BINARY="$(bin_path x86_64)/PaperShade"
else
    build_arch "$ARCH"
    NATIVE_BINARY="$(bin_path "$ARCH")/PaperShade"
fi

mkdir -p "$ROOT/dist" "$ICONSET" "$CLANG_MODULE_CACHE_PATH"
# Replace only this script's generated app, never the containing dist directory.
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
if [[ "$ARCH" == universal ]]; then
    xcrun lipo -create "$ARM_BINARY" "$INTEL_BINARY" -output "$APP/Contents/MacOS/PaperShade"
    xcrun lipo "$APP/Contents/MacOS/PaperShade" -verify_arch arm64 x86_64
else
    cp "$NATIVE_BINARY" "$APP/Contents/MacOS/PaperShade"
    xcrun lipo "$APP/Contents/MacOS/PaperShade" -verify_arch "$ARCH"
fi
sed "s/@VERSION@/$VERSION/g" "$ROOT/macOS/Info.plist" > "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"
"$SWIFT" "$ROOT/macOS/Tools/GenerateIcon.swift" "$ICONSET"
/usr/bin/iconutil --convert icns "$ICONSET" --output "$APP/Contents/Resources/PaperShade.icns"
/usr/bin/plutil -lint "$APP/Contents/Info.plist"
chmod 755 "$APP/Contents/MacOS/PaperShade"
chmod 644 "$APP/Contents/Info.plist" "$APP/Contents/PkgInfo" "$APP/Contents/Resources/PaperShade.icns"

# Ad-hoc signing is not Developer ID signing or notarization.
/usr/bin/codesign --force --sign - --timestamp=none "$APP"
/usr/bin/codesign --verify --deep --strict --verbose=2 "$APP"
rm -f "$ZIP"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$APP" "$ZIP"

DMG_STAGE="$(mktemp -d "$ROOT/.build/macos-dmg.XXXXXX")"
cleanup_dmg_stage() {
    # mktemp created this exact packaging directory; never remove a parent directory.
    rm -rf -- "$DMG_STAGE"
}
trap cleanup_dmg_stage EXIT
/usr/bin/ditto "$APP" "$DMG_STAGE/PaperShade.app"
ln -s /Applications "$DMG_STAGE/Applications"
/usr/bin/hdiutil create -volname PaperShade -srcfolder "$DMG_STAGE" \
    -format UDZO -ov "$DMG"
/usr/bin/hdiutil verify "$DMG"
printf '\nBuilt %s\nPackaged %s\nInstaller %s\nAd-hoc signed only; not notarized.\n' "$APP" "$ZIP" "$DMG"
