#!/bin/bash
# build.sh — Build, install, and register WaterMorphHelper (Morph companion) as a LaunchAgent.
# Run ONCE from the Companion/ directory after any update.
# The companion will then start automatically at every login.

set -e
cd "$(dirname "$0")"

APP_NAME="WaterMorphHelper"
BUNDLE="$APP_NAME.app"
SIGN_ID="Apple Development: graux.sebastien@gmail.com (83FP28KK87)"
LABEL="ai.95ent.watermorph.helper"

AU_HELPERS="$HOME/Library/Audio/Plug-Ins/Components/Morph.component/Contents/Helpers"
LAUNCH_AGENTS="$HOME/Library/LaunchAgents"
PLIST_DST="$LAUNCH_AGENTS/$LABEL.plist"
HELPER_EXE="$AU_HELPERS/$BUNDLE/Contents/MacOS/$APP_NAME"

# ── 1. Compile ────────────────────────────────────────────────────────────────
echo "==> Compiling $APP_NAME (Swift)..."
swiftc \
    -O \
    -target arm64-apple-macos12.0 \
    -framework Cocoa \
    -framework AVFoundation \
    -framework UniformTypeIdentifiers \
    -framework WebKit \
    -o "$APP_NAME" \
    WaterMorphCompanion.swift

# ── 2. Bundle ─────────────────────────────────────────────────────────────────
echo "==> Packaging as .app bundle..."
rm -rf "$BUNDLE" 2>/dev/null || sudo rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS"
mkdir -p "$BUNDLE/Contents/Resources"
cp "$APP_NAME" "$BUNDLE/Contents/MacOS/"
cp Info.plist  "$BUNDLE/Contents/"
rm -f "$APP_NAME"

# ── 3. Sign ───────────────────────────────────────────────────────────────────
echo "==> Signing..."
codesign \
    --sign "$SIGN_ID" \
    --force \
    "$BUNDLE"

# ── 4. Install into AU bundle ─────────────────────────────────────────────────
echo "==> Installing into AU bundle helpers..."
mkdir -p "$AU_HELPERS"
# sudo fallback in case a previous pkg installer left files owned by root.
rm -rf "$AU_HELPERS/$BUNDLE" 2>/dev/null || sudo rm -rf "$AU_HELPERS/$BUNDLE"
cp -R "$BUNDLE" "$AU_HELPERS/"

# ── 5. Install LaunchAgent (auto-start at login) ──────────────────────────────
echo "==> Installing LaunchAgent..."
mkdir -p "$LAUNCH_AGENTS"

# Unload any previous version first (ignore errors if not loaded)
launchctl unload "$PLIST_DST" 2>/dev/null || true

# Write plist with the real binary path
sed "s|HELPER_PATH_PLACEHOLDER|$HELPER_EXE|g" \
    ai.95ent.watermorph.helper.plist > "$PLIST_DST"

# Load and start immediately (no reboot needed)
launchctl load -w "$PLIST_DST"

# ── 6. Build user-facing installer package ────────────────────────────────────
echo "==> Building installer package..."
PKG_ROOT="$(mktemp -d)/payload"
APP_SUPPORT="$PKG_ROOT/Library/Application Support/Water"
mkdir -p "$APP_SUPPORT"
cp -R "$BUNDLE" "$APP_SUPPORT/"

pkgbuild \
    --identifier "ai.95ent.watermorph.companion" \
    --version "1.0.0" \
    --root "$PKG_ROOT" \
    --install-location "/" \
    --scripts "scripts" \
    "WaterMorphCompanion.pkg"

rm -rf "$(dirname "$PKG_ROOT")"

echo ""
echo "✓ Morph companion installed and running."
echo "  App:         $AU_HELPERS/$BUNDLE"
echo "  LaunchAgent: $PLIST_DST"
echo "  TCP port:    59812 (localhost)"
echo "  Log:         /tmp/water-morph-helper.log"
echo ""
echo "✓ Installer package built."
echo "  Package: $(pwd)/WaterMorphCompanion.pkg"
echo "  → Deploy: cp WaterMorphCompanion.pkg <water-repo>/apps/web/public/downloads/morph-companion.pkg"
