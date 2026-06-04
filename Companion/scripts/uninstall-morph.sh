#!/bin/bash
# uninstall-morph.sh — Remove Water Morph plugin (Mac).
# Double-click "Uninstall Water Morph.app" or run:
#   bash ~/Library/Application\ Support/Water/uninstall-morph.sh

LABEL="ai.95ent.watermorph.helper"
AU="$HOME/Library/Audio/Plug-Ins/Components/Morph.component"
VST3="$HOME/Library/Audio/Plug-Ins/VST3/Morph.vst3"
LAUNCH_PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
APP_SUPPORT="$HOME/Library/Application Support/Water"
PKG_RECEIPT_AU="/var/db/receipts/ai.95ent.morph.au.bom"
PKG_RECEIPT_VST3="/var/db/receipts/ai.95ent.morph.vst3.bom"
PKG_RECEIPT_COMPANION="/var/db/receipts/ai.95ent.watermorph.companion.bom"

removed=0

# Stop LaunchAgent daemon
if launchctl list "$LABEL" &>/dev/null 2>&1; then
    launchctl unload -w "$LAUNCH_PLIST" 2>/dev/null || true
fi
[ -f "$LAUNCH_PLIST" ] && rm -f "$LAUNCH_PLIST" && ((removed++))

# Remove AU
[ -d "$AU" ] && rm -rf "$AU" && ((removed++))

# Remove VST3
[ -d "$VST3" ] && rm -rf "$VST3" && ((removed++))

# Remove Application Support (helper + uninstaller itself)
[ -d "$APP_SUPPORT" ] && rm -rf "$APP_SUPPORT" && ((removed++))

# Remove pkg receipts (prevents "already installed" confusion on reinstall)
for f in "$PKG_RECEIPT_AU" "$PKG_RECEIPT_VST3" "$PKG_RECEIPT_COMPANION"; do
    plist="${f%.bom}.plist"
    [ -f "$f" ]    && sudo rm -f "$f"    2>/dev/null || true
    [ -f "$plist" ] && sudo rm -f "$plist" 2>/dev/null || true
done

if [ "$removed" -gt 0 ]; then
    osascript -e 'display dialog "Water Morph a été désinstallé.\nRelancez votre DAW pour finaliser." buttons {"OK"} default button 1 with title "Water Morph" with icon note' 2>/dev/null || \
    echo "Water Morph uninstalled. Restart your DAW."
else
    osascript -e 'display dialog "Water Morph ne semble pas installé sur cette machine." buttons {"OK"} default button 1 with title "Water Morph" with icon caution' 2>/dev/null || \
    echo "Water Morph does not appear to be installed."
fi
