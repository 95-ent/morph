#!/bin/bash
# ship-mac-bundles.sh — The Gatekeeper-clean Mac release path that does NOT need
# a "Developer ID Installer" cert (which we do not have yet).
#
# What it does:
#   1. Signs the UNIVERSAL VST3 + AU from build-mac artefacts (Developer ID
#      Application, hardened runtime, entitlements, secure timestamp).
#   2. Notarizes EACH bundle (ditto zip -> notarytool submit --wait).
#   3. Staples the notarization ticket onto each bundle (offline-valid).
#   4. Installs the stapled bundles into ~/Library/Audio/Plug-Ins (local test).
#   5. Builds a Developer-ID-Application-signed, notarized, stapled DMG that
#      carries both stapled bundles + the signed companion app + a README.
#
# Credentials come from AURA_SYSTEM/.env (never hardcoded, never committed):
#   notarytool: prefers App Store Connect API key, falls back to apple-id+pw.
#
# USAGE:  bash ship-mac-bundles.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ENV_FILE="${MORPH_ENV_FILE:-$HOME/GRAUX Dropbox/Sébastien Graux/GRAUX_SYSTEM/02_Tech/AURA_SYSTEM/.env}"
[ -f "$ENV_FILE" ] || ENV_FILE="$HOME/Dropbox-GRAUX/Sébastien Graux/GRAUX_SYSTEM/02_Tech/AURA_SYSTEM/.env"
[ -f "$ENV_FILE" ] || { echo "No .env found"; exit 1; }

get_env() {
    local v
    v=$(grep -m1 "^$1=" "$ENV_FILE" 2>/dev/null | cut -d= -f2- || true)
    if [ -z "$v" ] && [ -n "${2:-}" ]; then
        v=$(grep -m1 "^$2=" "$ENV_FILE" 2>/dev/null | cut -d= -f2- || true)
    fi
    printf '%s' "$v"
}

TEAM_ID="$(get_env TEAM_ID MORPH_TEAM_ID)"
APPLE_ID="$(get_env APPLE_ID MORPH_APPLE_ID)"
APP_PW="$(get_env APP_SPECIFIC_PASSWORD MORPH_APP_SPECIFIC_PASSWORD)"
ASC_KEY_ID="$(get_env APP_STORE_CONNECT_KEY_ID)"
ASC_ISSUER="$(get_env APP_STORE_CONNECT_ISSUER_ID)"
ASC_KEY_PATH="$(get_env APP_STORE_CONNECT_API_KEY_PATH)"
# APP_STORE_CONNECT_API_KEY_PATH is stored RELATIVE to AURA_SYSTEM (the .env dir).
# Resolve it to an absolute path so the notarize() -f check passes and we use the
# API key instead of silently falling back to apple-id+pw (which left the DMG
# un-notarized on the Jun 4 run).
case "$ASC_KEY_PATH" in
    "" ) : ;;
    /* ) : ;;
    * ) ASC_KEY_PATH="$(dirname "$ENV_FILE")/$ASC_KEY_PATH" ;;
esac
[ -n "$TEAM_ID" ] || { echo "Missing TEAM_ID in .env"; exit 1; }

# Resolve the exact Developer ID Application identity (match strictly on Team ID).
SIGN_APP="$( { security find-identity -v -p codesigning \
    | grep "Developer ID Application" | grep "($TEAM_ID)" \
    | head -1 | sed -E 's/^[[:space:]]*[0-9]+\)[[:space:]]+[0-9A-F]+[[:space:]]+"(.*)"$/\1/'; } 2>/dev/null || true)"
[ -n "$SIGN_APP" ] || { echo "BLOCKED: no Developer ID Application identity for Team $TEAM_ID."; exit 1; }
echo "Signing identity : $SIGN_APP"

VERSION="$(grep -Eo 'project\(Morph VERSION [0-9]+\.[0-9]+\.[0-9]+' "$HERE/CMakeLists.txt" | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+')"
[ -n "$VERSION" ] || { echo "Cannot parse version"; exit 1; }
echo "Version          : $VERSION"

BUILD_VST3="$HERE/build-mac/Morph_artefacts/Release/VST3/Morph.vst3"
BUILD_AU="$HERE/build-mac/Morph_artefacts/Release/AU/Morph.component"
VST3_ENT="$HERE/build-mac/Morph_artefacts/JuceLibraryCode/Morph_VST3.entitlements"
AU_ENT="$HERE/build-mac/Morph_artefacts/JuceLibraryCode/Morph_AU.entitlements"
[ -d "$BUILD_VST3" ] || { echo "Missing $BUILD_VST3 (build first)"; exit 1; }
[ -d "$BUILD_AU" ]   || { echo "Missing $BUILD_AU (build first)"; exit 1; }

OUT="$HERE/dist/signed_universal"
rm -rf "$OUT"; mkdir -p "$OUT"
cp -R "$BUILD_VST3" "$OUT/Morph.vst3"
cp -R "$BUILD_AU"   "$OUT/Morph.component"

# notarytool auth args (prefer API key — proven to work for this org).
notarize() {
    local target="$1"
    if [ -n "$ASC_KEY_ID" ] && [ -n "$ASC_ISSUER" ] && [ -f "$ASC_KEY_PATH" ]; then
        xcrun notarytool submit "$target" --key "$ASC_KEY_PATH" --key-id "$ASC_KEY_ID" --issuer "$ASC_ISSUER" --wait
    else
        xcrun notarytool submit "$target" --apple-id "$APPLE_ID" --password "$APP_PW" --team-id "$TEAM_ID" --wait
    fi
}

sign_bundle() {
    local b="$1" ent="$2"
    # inside-out: nested executables/dylibs first, then the bundle.
    find "$b/Contents" -type f \( -name "*.dylib" -o -perm -111 \) 2>/dev/null | while read -r f; do
        case "$f" in *Info.plist|*.plist|*.nib|*.icns) continue;; esac
        codesign --force --timestamp --options runtime --sign "$SIGN_APP" "$f" 2>/dev/null || true
    done
    if [ -f "$ent" ]; then
        codesign --force --timestamp --options runtime --entitlements "$ent" --sign "$SIGN_APP" "$b"
    else
        codesign --force --timestamp --options runtime --sign "$SIGN_APP" "$b"
    fi
    codesign --verify --strict --verbose=2 "$b"
}

echo "==> [1/5] Signing universal bundles..."
sign_bundle "$OUT/Morph.vst3" "$VST3_ENT"
sign_bundle "$OUT/Morph.component" "$AU_ENT"

echo "==> [2/5] Notarizing each bundle (1-5 min each)..."
( cd "$OUT" && ditto -c -k --keepParent "Morph.vst3" "Morph_vst3.zip" )
notarize "$OUT/Morph_vst3.zip"
( cd "$OUT" && ditto -c -k --keepParent "Morph.component" "Morph_component.zip" )
notarize "$OUT/Morph_component.zip"

echo "==> [3/5] Stapling tickets onto the bundles..."
xcrun stapler staple "$OUT/Morph.vst3"
xcrun stapler staple "$OUT/Morph.component"
rm -f "$OUT/Morph_vst3.zip" "$OUT/Morph_component.zip"

echo "==> [4/5] Installing stapled bundles to ~/Library/Audio/Plug-Ins (local test)..."
mkdir -p "$HOME/Library/Audio/Plug-Ins/VST3" "$HOME/Library/Audio/Plug-Ins/Components"
rm -rf "$HOME/Library/Audio/Plug-Ins/VST3/Morph.vst3" "$HOME/Library/Audio/Plug-Ins/Components/Morph.component"
cp -R "$OUT/Morph.vst3" "$HOME/Library/Audio/Plug-Ins/VST3/Morph.vst3"
cp -R "$OUT/Morph.component" "$HOME/Library/Audio/Plug-Ins/Components/Morph.component"

echo "==> [5/5] Building signed + notarized DMG..."
STAGE="$(mktemp -d)"
mkdir -p "$STAGE/Water Morph"
cp -R "$OUT/Morph.vst3" "$STAGE/Water Morph/Morph.vst3"
cp -R "$OUT/Morph.component" "$STAGE/Water Morph/Morph.component"
# Include the companion app ONLY if it is Developer-ID signed (a Development-signed
# app would fail DMG notarization). Otherwise ship plugin-only; companion ships
# via its own notarized build.
COMP_APP="$HERE/Companion/WaterMorphHelper.app"
if [ -d "$COMP_APP" ] && codesign -dv "$COMP_APP" 2>&1 | grep -q "Developer ID Application"; then
    cp -R "$COMP_APP" "$STAGE/Water Morph/Water Morph.app"
    echo "    included Developer-ID companion app in DMG"
else
    echo "    companion app NOT Developer-ID signed -> plugin-only DMG"
fi
cat > "$STAGE/Water Morph/INSTALL.txt" <<'TXT'
WATER MORPH — install (macOS)

1. Move "Morph.vst3"     -> ~/Library/Audio/Plug-Ins/VST3/
2. Move "Morph.component"-> ~/Library/Audio/Plug-Ins/Components/
   (If a folder does not exist, create it.)
3. If included, move "Water Morph.app" -> /Applications and open it once.
4. Restart your DAW (Logic / FL / Ableton) and rescan plugins.

Signed + notarized by 95 Entertainment LLC.
TXT
DMG_RAW="$HERE/dist/Water_Morph_${VERSION}_macOS.dmg.raw.dmg"
DMG_OUT="$HERE/dist/Water_Morph_${VERSION}_macOS.dmg"
rm -f "$DMG_RAW" "$DMG_OUT"
hdiutil create -volname "Water Morph" -srcfolder "$STAGE/Water Morph" -ov -format UDZO "$DMG_OUT" >/dev/null
codesign --force --timestamp --sign "$SIGN_APP" "$DMG_OUT"
echo "    notarizing DMG..."
notarize "$DMG_OUT"
xcrun stapler staple "$DMG_OUT"
rm -rf "$STAGE"

echo ""
echo "==> VERIFICATION"
echo "--- VST3 ---";       spctl -a -vvv -t install "$OUT/Morph.vst3" 2>&1 | head -3
echo "--- AU ---";         spctl -a -vvv -t install "$OUT/Morph.component" 2>&1 | head -3
echo "--- DMG ---";        spctl -a -vvv -t open --context context:primary-signature "$DMG_OUT" 2>&1 | head -3
echo "--- installed VST3 stapled? ---"; xcrun stapler validate "$HOME/Library/Audio/Plug-Ins/VST3/Morph.vst3" 2>&1 | tail -1
echo ""
echo "DONE v$VERSION. Distributable: $DMG_OUT"
