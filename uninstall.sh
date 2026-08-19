#!/usr/bin/env bash
# Remove everything install.sh set up.
set -u
say(){ printf "\n\033[1;36m==> %s\033[0m\n" "$*"; }
say "Removing printer queue..."
lpadmin -x HP_Laser_1008a 2>/dev/null || true
say "Removing the root daemon + native filter (needs sudo)..."
sudo launchctl bootout system/com.hpl1008.daemon 2>/dev/null || true
sudo rm -f /Library/LaunchDaemons/com.hpl1008.daemon.plist \
           /usr/local/bin/hpl1008-daemon \
           /usr/libexec/cups/filter/rastertoqpdl
say "Removing support files..."
rm -rf "$HOME/.hp1008"
echo
echo "Done. (libusb was left installed; 'brew uninstall libusb' to remove it.)"
