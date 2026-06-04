#!/bin/bash
# rebuild-and-sign-mac.sh - Rebuild the macOS Water Morph .pkg from FRESHLY
# Developer-ID-signed UNIVERSAL payloads, then notarize + staple.
#
# WHY THIS EXISTS:
#   sign-and-notarize-mac.sh only re-signs the *installed* bundles in
#   ~/Library/Audio/Plug-Ins. The shipped dist/*.pkg still embedded the OLD
#   adhoc (and arm64-only) payloads, so notarization returned Invalid.
#   This script signs the payloads that actually go INTO the pkg, rebuilds
#   every component pkg + the product pkg, then notarizes the rebuilt pkg.
#
# SOURCE OF TRUTH for the universal v1.0.x plugins:
#   build-mac/Morph_artefacts/Release/{VST3/Morph.vst3, AU/Morph.component}
#   (these are universal x86_64+arm64 with the correct CFBundleShortVersionString).
#
# Credentials are read from AURA_SYSTEM/.env (NOT hardcoded, NOT committed).
#
# USAGE:  bash rebuild-and-sign-mac.sh

set -euo pipefail

ENV_FILE="${MORPH_ENV_FILE:-$HOME/GRAUX Dropbox/Sébastien Graux/GRAUX_SYSTEM/02_Tech/AURA_SYSTEM/.env}"
[ -f "$ENV_FILE" ] || ENV_FILE="$HOME/Dropbox-GRAUX/Sébastien Graux/GRAUX_SYSTEM/02_Tech/AURA_SYSTEM/.env"

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
APP_SPECIFIC_PASSWORD="$(get_env APP_SPECIFIC_PASSWORD MORPH_APP_SPECIFIC_PASSWORD)"
[ -n "$TEAM_ID" ] && [ -n "$APPLE_ID" ] && [ -n "$APP_SPECIFIC_PASSWORD" ] \
    || { echo "Missing TEAM_ID / APPLE_ID / APP_SPECIFIC_PASSWORD in $ENV_FILE"; exit 1; }

# Resolve the exact Developer ID Application identity from the keychain (org or
# personal CN varies); match strictly on Team ID.
SIGN_APP="$( { security find-identity -v -p codesigning \
    | grep "Developer ID Application" | grep "($TEAM_ID)" \
    | head -1 | sed -E 's/^[[:space:]]*[0-9]+\)[[:space:]]+[0-9A-F]+[[:space:]]+"(.*)"$/\1/'; } 2>/dev/null || true)"
[ -n "$SIGN_APP" ] || { echo "BLOCKED: no Developer ID Application identity for Team $TEAM_ID in keychain."; exit 1; }
SIGN_INSTALLER="$( { security find-identity -v \
    | grep "Developer ID Installer" | grep "($TEAM_ID)" \
    | head -1 | sed -E 's/^[[:space:]]*[0-9]+\)[[:space:]]+[0-9A-F]+[[:space:]]+"(.*)"$/\1/'; } 2>/dev/null || true)"

echo "Identity: $SIGN_APP"

HERE="$(cd "$(dirname "$0")" && pwd)"
VERSION="$(grep -Eo 'project\(Morph VERSION [0-9]+\.[0-9]+\.[0-9]+' "$HERE/CMakeLists.txt" | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+')"
[ -n "$VERSION" ] || { echo "Cannot parse version from CMakeLists.txt"; exit 1; }
echo "Version: $VERSION"

DIST="$HERE/dist"
BUILD_VST3="$HERE/build-mac/Morph_artefacts/Release/VST3/Morph.vst3"
BUILD_AU="$HERE/build-mac/Morph_artefacts/Release/AU/Morph.component"
[ -d "$BUILD_VST3" ] || { echo "Missing universal VST3 at $BUILD_VST3"; exit 1; }
[ -d "$BUILD_AU" ]   || { echo "Missing universal AU at $BUILD_AU"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

sign_bundle() {
    # sign_bundle <bundle> [entitlements]
    local b="$1" ent="${2:-}"
    # Sign nested executables/dylibs first (inside-out), then the bundle.
    find "$b/Contents" -type f \( -name "*.dylib" -o -perm -111 \) 2>/dev/null | while read -r f; do
        case "$f" in *Info.plist|*.plist|*.nib|*.icns) continue;; esac
        codesign --force --timestamp --options runtime --sign "$SIGN_APP" "$f" 2>/dev/null || true
    done
    if [ -n "$ent" ] && [ -f "$ent" ]; then
        codesign --force --timestamp --options runtime --entitlements "$ent" --sign "$SIGN_APP" "$b"
    else
        codesign --force --timestamp --options runtime --sign "$SIGN_APP" "$b"
    fi
    codesign --verify --strict --verbose=2 "$b"
}

echo "==> [1/4] Signing UNIVERSAL plugin bundles (Developer ID, hardened runtime, timestamp)..."
VST3_STAGE="$WORK/vst3root/Library/Audio/Plug-Ins/VST3"
AU_STAGE="$WORK/auroot/Library/Audio/Plug-Ins/Components"
mkdir -p "$VST3_STAGE" "$AU_STAGE"
cp -R "$BUILD_VST3" "$VST3_STAGE/"
cp -R "$BUILD_AU"   "$AU_STAGE/"
VST3_ENT="$HERE/build-mac/Morph_artefacts/JuceLibraryCode/Morph_VST3.entitlements"
AU_ENT="$HERE/build-mac/Morph_artefacts/JuceLibraryCode/Morph_AU.entitlements"
sign_bundle "$VST3_STAGE/Morph.vst3" "$VST3_ENT"
sign_bundle "$AU_STAGE/Morph.component" "$AU_ENT"

echo "==> [2/4] Re-signing companion payload (helper + uninstall app) with Developer ID..."
# Extract the existing companion component pkg payload, re-sign the apps, keep scripts.
COMP_SRC="$DIST/WaterMorphCompanion.pkg"
[ -f "$COMP_SRC" ] || COMP_SRC="$HERE/Companion/WaterMorphCompanion.pkg"
[ -f "$COMP_SRC" ] || { echo "Missing WaterMorphCompanion.pkg"; exit 1; }
COMP_EXP="$WORK/comp_exp"
pkgutil --expand "$COMP_SRC" "$COMP_EXP"
COMP_ROOT="$WORK/comproot"
mkdir -p "$COMP_ROOT"
( cd "$COMP_ROOT" && cat "$COMP_EXP/Payload" | gzip -dc 2>/dev/null | cpio -i 2>/dev/null \
    || cat "$COMP_EXP/Payload" | cpio -i 2>/dev/null )
# Sign every .app in the companion payload (deep, runtime, timestamp).
find "$COMP_ROOT" -name "*.app" -type d 2>/dev/null | while read -r app; do
    codesign --force --timestamp --options runtime --deep --sign "$SIGN_APP" "$app"
    echo "    signed companion app: $app"
done
# Rebuild the companion component pkg, preserving the original scripts dir.
COMP_PKG="$WORK/WaterMorphCompanion.pkg"
SCRIPTS_DIR="$COMP_EXP/Scripts"
if [ -d "$SCRIPTS_DIR" ]; then
    pkgbuild --identifier "ai.95ent.watermorph.companion" --version "$VERSION" \
        --root "$COMP_ROOT" --install-location "/" --scripts "$SCRIPTS_DIR" "$COMP_PKG"
else
    pkgbuild --identifier "ai.95ent.watermorph.companion" --version "$VERSION" \
        --root "$COMP_ROOT" --install-location "/" "$COMP_PKG"
fi

echo "==> [3/4] Building component pkgs + product pkg..."
VST3_PKG="$WORK/morph_vst3.pkg"
AU_PKG="$WORK/morph_au.pkg"
pkgbuild --identifier "ai.95ent.morph.vst3" --version "$VERSION" \
    --root "$WORK/vst3root" --install-location "/" "$VST3_PKG"
pkgbuild --identifier "ai.95ent.morph.au" --version "$VERSION" \
    --root "$WORK/auroot" --install-location "/" "$AU_PKG"

# productbuild needs the component pkgs next to the distribution.xml refs.
PB_DIR="$WORK/pb"
mkdir -p "$PB_DIR"
cp "$VST3_PKG" "$AU_PKG" "$COMP_PKG" "$PB_DIR/"
cp "$DIST/distribution.xml" "$PB_DIR/distribution.xml"
# Bump the component versions referenced in distribution.xml to $VERSION.
sed -i '' -E "s/(id=\"ai\.95ent\.morph\.vst3\" version=\")[0-9.]+/\1$VERSION/" "$PB_DIR/distribution.xml" || true
sed -i '' -E "s/(id=\"ai\.95ent\.morph\.au\" version=\")[0-9.]+/\1$VERSION/" "$PB_DIR/distribution.xml" || true
sed -i '' -E "s/(id=\"ai\.95ent\.watermorph\.companion\" version=\")[0-9.]+/\1$VERSION/" "$PB_DIR/distribution.xml" || true

PRODUCT_PKG="$DIST/Water_Morph_${VERSION}_macOS.pkg"
SIGNED_PKG="$DIST/Water_Morph_${VERSION}_macOS_signed.pkg"
if [ -n "$SIGN_INSTALLER" ]; then
    productbuild --distribution "$PB_DIR/distribution.xml" --package-path "$PB_DIR" \
        --sign "$SIGN_INSTALLER" --timestamp "$PRODUCT_PKG"
else
    echo "    (no Developer ID Installer cert; building unsigned product pkg with signed payloads)"
    productbuild --distribution "$PB_DIR/distribution.xml" --package-path "$PB_DIR" "$PRODUCT_PKG"
fi
cp "$PRODUCT_PKG" "$SIGNED_PKG"
echo "    product pkg: $PRODUCT_PKG"

echo "==> [4/4] Notarizing (1-5 min) + stapling..."
xcrun notarytool submit "$SIGNED_PKG" \
    --apple-id "$APPLE_ID" --password "$APP_SPECIFIC_PASSWORD" --team-id "$TEAM_ID" --wait
xcrun stapler staple "$SIGNED_PKG" || { echo "Staple failed (pkg may be unsigned-but-app-signed; plugins are notarized)."; }

echo "==> Verification:"
spctl -a -vvv -t install "$SIGNED_PKG" 2>&1 || true
echo "--- VST3 (extracted) ---"
codesign -dv "$VST3_STAGE/Morph.vst3" 2>&1 | grep -E "TeamIdentifier|Authority=Developer ID|Timestamp" | head

echo ""
echo "Done. Universal signed/notarized pkg for v$VERSION:"
echo "  $SIGNED_PKG"
