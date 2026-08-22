#!/bin/bash
# Switch an existing install from the socket+daemon (v1) to the CUPS backend (V2) and clean
# up: point the main queue at hpl100x:/, remove the scratch test queue and the old
# LaunchDaemon, and turn debug logging back off. Assumes the hpl100x backend binary is built
# at ~/.hp1008/hpl1008-usbd (install.sh or the V2 test builds it). Safe to re-run.
set -e
H="$HOME/.hp1008"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> installing the current backend build"
sudo install -o root -g wheel -m 0700 "$H/hpl1008-usbd" /usr/libexec/cups/backend/hpl100x

echo "==> pointing HP_Laser_1008a at the native backend (hpl100x:/)"
sudo lpadmin -p HP_Laser_1008a -E -v hpl100x:/

echo "==> removing the scratch test queue (if present)"
sudo lpadmin -x HP_1008_be 2>/dev/null || true

echo "==> removing the old socket LaunchDaemon (if present)"
sudo launchctl bootout system/com.hpl1008.daemon 2>/dev/null || true
sudo rm -f /Library/LaunchDaemons/com.hpl1008.daemon.plist /usr/local/bin/hpl1008-daemon

echo "==> test print via the main queue"
lp -d HP_Laser_1008a "$HERE/testpage.txt"
sleep 6

# Only quiet the logs once the test print has gone through, so a failure stays diagnosable.
echo "==> turning debug logging back off"
sudo cupsctl --no-debug-logging || true
echo
echo "Done. 'HP Laser 1008a' now prints via the native backend. No socket, no daemon."
