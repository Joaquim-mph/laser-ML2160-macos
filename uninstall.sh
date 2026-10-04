#!/usr/bin/env bash
# Remove everything install.sh set up.
set -u
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
say(){ printf "\n\033[1;36m==> %s\033[0m\n" "$*"; }
say "Removing printer queue..."
lpadmin -x HP_Laser_1008a 2>/dev/null || true
say "Re-enabling macOS's IPP-over-USB bridge (install.sh disabled it)..."
sudo bash "$REPO_DIR/tools/disable-ippusb.sh" --undo 2>/dev/null || true
say "Removing the native CUPS backend + filter (needs sudo)..."
sudo launchctl bootout system/com.hpl1008.daemon 2>/dev/null || true
sudo rm -f /Library/LaunchDaemons/com.hpl1008.daemon.plist \
           /usr/local/bin/hpl1008-daemon \
           /usr/libexec/cups/backend/hpl100x \
           /usr/libexec/cups/filter/rastertoqpdl
say "Removing support files..."
rm -rf "$HOME/.hp1008"
echo
echo "Done. Nothing else to uninstall (the driver uses only Apple system frameworks)."
