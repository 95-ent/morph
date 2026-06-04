#!/bin/bash
# build-dist.sh — Build the Water Morph companion as a DISTRIBUTABLE app:
# UNIVERSAL (arm64 + x86_64), Developer ID Application signed, hardened runtime,
# notarized + stapled. Unlike build.sh (local dev: arm64-only, Development-signed,
# installs a LaunchAgent), this produces an app that passes Gatekeeper on any Mac.
#
# Output: Companion/WaterMorphHelper.app  (notarized, stapled)
# Credentials from AURA_SYSTEM/.env (notarytool: API key preferred).
#
# USAGE:  bash build-dist.sh
set -euo pipefail
cd "$(dirname "$0")"

ENV_FILE="${MORPH_ENV_FILE:-$HOME/GRAUX Dropbox/Sébastien Graux/GRAUX_SYSTEM/02_Tech/AURA_SYSTEM/.env}"
[ -f "$ENV_FILE" ] || ENV_FILE="$HOME/Dropbox-GRAUX/Sébastien Graux/GRAUX_SYSTEM/02_Tech/AURA_SYSTEM/.env"
get_env() { grep -m1 "^$1=" "$ENV_FILE" 2>/dev/null | cut -d= -f2- || true; }

TEAM_ID="$(get_env TEAM_ID)"; [ -n "$TEAM_ID" ] || TEAM_ID="$(get_env MORPH_TEAM_ID)"
APPLE_ID="$(get_env APPLE_ID)"
APP_PW="$(get_env APP_SPECIFIC_PASSWORD)"
ASC_KEY_ID="$(get_env APP_STORE_CONNECT_KEY_ID)"
ASC_ISSUER="$(get_env APP_STORE_CONNECT_ISSUER_ID)"
ASC_KEY_PATH="$(get_env APP_STORE_CONNECT_API_KEY_PATH)"
[ -n "$TEAM_ID" ] || { echo "Missing TEAM_ID"; exit 1; }

SIGN_APP="$( { security find-identity -v -p codesigning \
    | grep "Developer ID Application" | grep "($TEAM_ID)" \
    | head -1 | sed -E 's/^[[:space:]]*[0-9]+\)[[:space:]]+[0-9A-F]+[[:space:]]+"(.*)"$/\1/'; } 2>/dev/null || true)"
[ -n "$SIGN_APP" ] || { echo "BLOCKED: no Developer ID Application identity for Team $TEAM_ID."; exit 1; }
echo "Identity: $SIGN_APP"

APP_NAME="WaterMorphHelper"
BUNDLE="$APP_NAME.app"

echo "==> [1/4] Compiling universal (arm64 + x86_64)..."
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
for arch in arm64 x86_64; do
    swiftc -O -target ${arch}-apple-macos12.0 \
        -framework Cocoa -framework AVFoundation \
        -framework UniformTypeIdentifiers -framework WebKit \
        -o "$TMP/$APP_NAME.$arch" WaterMorphCompanion.swift
done
lipo -create "$TMP/$APP_NAME.arm64" "$TMP/$APP_NAME.x86_64" -output "$TMP/$APP_NAME"
echo "    archs: $(lipo -archs "$TMP/$APP_NAME")"

echo "==> [2/4] Bundling .app..."
rm -rf "$BUNDLE" 2>/dev/null || sudo rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"
cp "$TMP/$APP_NAME"   "$BUNDLE/Contents/MacOS/"
cp Info.plist         "$BUNDLE/Contents/"
[ -f WaterMorph.icns ] && cp WaterMorph.icns "$BUNDLE/Contents/Resources/" || true

echo "==> [3/4] Signing (Developer ID, hardened runtime, timestamp)..."
codesign --force --timestamp --options runtime --sign "$SIGN_APP" "$BUNDLE"
codesign --verify --strict --verbose=2 "$BUNDLE"

echo "==> [4/4] Notarizing + stapling..."
ZIP="$TMP/$APP_NAME.zip"
ditto -c -k --keepParent "$BUNDLE" "$ZIP"
if [ -n "$ASC_KEY_ID" ] && [ -n "$ASC_ISSUER" ] && [ -f "$ASC_KEY_PATH" ]; then
    xcrun notarytool submit "$ZIP" --key "$ASC_KEY_PATH" --key-id "$ASC_KEY_ID" --issuer "$ASC_ISSUER" --wait
else
    xcrun notarytool submit "$ZIP" --apple-id "$APPLE_ID" --password "$APP_PW" --team-id "$TEAM_ID" --wait
fi
xcrun stapler staple "$BUNDLE"

echo ""
echo "==> VERIFICATION"
spctl -a -vvv -t exec "$BUNDLE" 2>&1 | head -3
codesign -dv "$BUNDLE" 2>&1 | grep -E "TeamIdentifier|Authority=Developer ID|flags" | head -3
echo ""
echo "DONE. Notarized universal companion: $(pwd)/$BUNDLE"
