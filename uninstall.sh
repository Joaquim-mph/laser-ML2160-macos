#!/usr/bin/env bash
# Remove everything install.sh set up.
set -u
say(){ printf "\n\033[1;36m==> %s\033[0m\n" "$*"; }

say "Removing printer queue…"
lpadmin -x HP_Laser_1008a 2>/dev/null || true

say "Stopping + removing the root daemon (needs sudo)…"
sudo launchctl bootout system/com.hpl1008.daemon 2>/dev/null || true
sudo rm -f /Library/LaunchDaemons/com.hpl1008.daemon.plist \
           /usr/local/bin/hpl1008-daemon \
           /usr/libexec/cups/filter/hpl1008-raster

say "Removing colima autostart…"
launchctl bootout "gui/$(id -u)/com.hpl1008.colima" 2>/dev/null || true
rm -f "$HOME/Library/LaunchAgents/com.hpl1008.colima.plist"

say "Removing support files + container image…"
rm -rf "$HOME/.hp1008"
docker rmi hp-spl 2>/dev/null || true

echo
echo "Done. (colima, docker, libusb were left installed - 'brew uninstall' them if you like.)"
echo "To stop the Linux VM entirely:  colima stop"
