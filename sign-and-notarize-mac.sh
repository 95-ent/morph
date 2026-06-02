#!/bin/bash
# sign-and-notarize-mac.sh - Sign + notarize the macOS Water Morph plugins + installer.
#
# Credentials are read from AURA_SYSTEM/.env (NOT hardcoded, NOT committed):
#   APPLE_ID, APP_SPECIFIC_PASSWORD, TEAM_ID
# (MORPH_-prefixed aliases are also accepted.)
#
# ONE-TIME PREREQUISITE (paid Apple Developer Program required):
#   1. developer.apple.com -> Certificates -> create "Developer ID Application"
#   2. developer.apple.com -> Certificates -> create "Developer ID Installer"
#      (or: Xcode -> Settings -> Accounts -> Apple ID -> Manage Certificates -> +)
#   3. Double-click both .cer to install them in the login keychain
#   4. App-specific password is already stored in AURA_SYSTEM/.env
#
# A FREE "Personal Team" CANNOT create Developer ID certs or notarize.
# If `security find-identity` shows only "Apple Development", this script exits 1
# with the exact manual action needed.
#
# USAGE:  bash sign-and-notarize-mac.sh

set -euo pipefail

# -- Load credentials from .env (only the keys we need; ignore the rest) -------
ENV_FILE="${MORPH_ENV_FILE:-$HOME/GRAUX Dropbox/Sébastien Graux/GRAUX_SYSTEM/02_Tech/AURA_SYSTEM/.env}"
[ -f "$ENV_FILE" ] || ENV_FILE="$HOME/Dropbox-GRAUX/Sébastien Graux/GRAUX_SYSTEM/02_Tech/AURA_SYSTEM/.env"

get_env() {
    # get_env KEY [FALLBACK_KEY] -> prints value, no eval of the file
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

if [ -z "$TEAM_ID" ] || [ -z "$APPLE_ID" ] || [ -z "$APP_SPECIFIC_PASSWORD" ]; then
    echo "Missing TEAM_ID / APPLE_ID / APP_SPECIFIC_PASSWORD in $ENV_FILE"
    exit 1
fi

SIGN_APP="Developer ID Application: Sébastien Graux ($TEAM_ID)"
SIGN_INSTALLER="Developer ID Installer: Sébastien Graux ($TEAM_ID)"

# Version = single source of truth from CMakeLists.txt
HERE="$(cd "$(dirname "$0")" && pwd)"
VERSION="$(grep -Eo 'project\(Morph VERSION [0-9]+\.[0-9]+\.[0-9]+' "$HERE/CMakeLists.txt" | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+')"
[ -n "$VERSION" ] || { echo "Cannot parse version from CMakeLists.txt"; exit 1; }

DIST_DIR="$HERE/dist"
PKG_INPUT="$DIST_DIR/Water_Morph_${VERSION}_macOS.pkg"
PKG_SIGNED="$DIST_DIR/Water_Morph_${VERSION}_macOS_signed.pkg"

AU_BUNDLE="$HOME/Library/Audio/Plug-Ins/Components/Morph.component"
VST3_BUNDLE="$HOME/Library/Audio/Plug-Ins/VST3/Morph.vst3"

# -- Validate Developer ID certificates exist ----------------------------------
if ! security find-identity -v -p codesigning | grep -q "Developer ID Application"; then
    echo "BLOCKED: no 'Developer ID Application' identity in the keychain."
    echo "This Apple ID currently has only a free Personal Team, which cannot sign/notarize."
    echo "ONE-TIME MANUAL ACTION (CEO):"
    echo "  1. Confirm the paid Apple Developer Program membership is ACTIVE"
    echo "     (developer.apple.com/account -> Membership; accept any pending agreement)."
    echo "  2. Xcode -> Settings -> Accounts -> graux.sebastien@gmail.com"
    echo "     -> Manage Certificates -> + -> 'Developer ID Application'"
    echo "     (and again -> + -> 'Developer ID Installer')."
    echo "  3. Re-run this script."
    exit 1
fi

echo "Developer ID Application certificate found. Version=$VERSION"

# -- 1. Sign AU + VST3 (universal bundles) -------------------------------------
echo "==> Signing plugin binaries + bundles (Developer ID, hardened runtime, timestamp)..."
for bundle in "$AU_BUNDLE" "$VST3_BUNDLE"; do
    [ -d "$bundle" ] || { echo "  skip (not installed): $bundle"; continue; }
    inner="$bundle/Contents/MacOS/Morph"
    if [ -f "$inner" ]; then
        # Verify universal before signing
        archs="$(lipo -archs "$inner" 2>/dev/null || true)"
        case "$archs" in
            *arm64*x86_64*|*x86_64*arm64*) : ;;
            *) echo "  WARNING: $inner is not universal (archs: $archs). Rebuild with -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64'." ;;
        esac
        codesign --sign "$SIGN_APP" --force --timestamp --options runtime "$inner"
    fi
    codesign --sign "$SIGN_APP" --force --timestamp --options runtime "$bundle"
    echo "  signed: $bundle"
done

# -- 2. Sign the PKG with Developer ID Installer -------------------------------
if [ -f "$PKG_INPUT" ]; then
    echo "==> Signing PKG..."
    if security find-identity -v | grep -q "Developer ID Installer"; then
        productsign --sign "$SIGN_INSTALLER" --timestamp "$PKG_INPUT" "$PKG_SIGNED"
    else
        echo "  no Developer ID Installer cert; notarizing the unsigned-but-app-signed pkg copy."
        cp "$PKG_INPUT" "$PKG_SIGNED"
    fi
    echo "  signed PKG: $PKG_SIGNED"

    # -- 3. Notarize ------------------------------------------------------------
    echo "==> Submitting for notarization (1-5 min)..."
    xcrun notarytool submit "$PKG_SIGNED" \
        --apple-id "$APPLE_ID" \
        --password "$APP_SPECIFIC_PASSWORD" \
        --team-id "$TEAM_ID" \
        --wait

    # -- 4. Staple --------------------------------------------------------------
    echo "==> Stapling..."
    xcrun stapler staple "$PKG_SIGNED"

    echo "==> Gatekeeper assessment:"
    spctl -a -vvv -t install "$PKG_SIGNED" || true
else
    echo "No pkg at $PKG_INPUT; signed the installed bundles only."
fi

echo ""
echo "Done. Signed/notarized for version $VERSION."
echo "Verify: codesign -dv \"$VST3_BUNDLE\"  (TeamIdentifier should be $TEAM_ID, not adhoc)"
echo "        spctl -a -vvv \"$VST3_BUNDLE\"  (should say accepted)"
