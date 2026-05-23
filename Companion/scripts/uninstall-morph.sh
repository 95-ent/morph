#!/bin/bash
# uninstall-morph.sh — Remove Water Morph plugin (Mac).
# Run from Terminal: bash ~/Library/Application\ Support/Water/uninstall-morph.sh
# Removes: AU component, VST3, LaunchAgent, helper app.

set -e

LABEL="ai.95ent.watermorph.helper"
AU="$HOME/Library/Audio/Plug-Ins/Components/Morph.component"
VST3="$HOME/Library/Audio/Plug-Ins/VST3/Morph.vst3"
LAUNCH_PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
APP_SUPPORT="$HOME/Library/Application Support/Water"

echo "Uninstalling Water Morph..."

# Stop the helper daemon
if launchctl list "$LABEL" &>/dev/null; then
    launchctl unload -w "$LAUNCH_PLIST" 2>/dev/null || true
    echo "  ✓ LaunchAgent stopped"
fi

# Remove plist
[ -f "$LAUNCH_PLIST" ] && rm -f "$LAUNCH_PLIST" && echo "  ✓ LaunchAgent plist removed"

# Remove AU plugin
if [ -d "$AU" ]; then
    rm -rf "$AU"
    echo "  ✓ AU plugin removed"
fi

# Remove VST3 plugin
if [ -d "$VST3" ]; then
    rm -rf "$VST3"
    echo "  ✓ VST3 plugin removed"
fi

# Remove Application Support folder (keeps user prefs out of scope — only removes the app)
if [ -d "$APP_SUPPORT/WaterMorphHelper.app" ]; then
    rm -rf "$APP_SUPPORT/WaterMorphHelper.app"
    echo "  ✓ Helper app removed"
fi

echo ""
echo "Water Morph uninstalled. Restart your DAW to complete."
