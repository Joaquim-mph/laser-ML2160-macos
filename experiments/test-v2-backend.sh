#!/bin/bash
# V2 experiment: can a CUPS backend do IOKit USB directly (in the backend sandbox)?
# If yes, we can delete the localhost socket + LaunchDaemon entirely.
# Installs the SAME native binary as a backend named hpl100x on a scratch queue.
set -e
H="$HOME/.hp1008"
echo "Installing hpl100x backend (0700 root -> runs as root)..."
sudo install -o root -g wheel -m 0700 "$H/hpl1008-usbd" /usr/libexec/cups/backend/hpl100x
echo "Creating scratch queue on the custom backend..."
lpadmin -p HP_1008_be -E -v hpl100x:/ -P "$H/laser10x.ppd" -o printer-is-shared=false
echo "Test print..."
lp -d HP_1008_be "$H/testpage.txt"
echo
echo "If a clean page prints, the backend sandbox ALLOWS IOKit USB -> we drop socket+daemon."
echo "If it stays queued / errors, the sandbox blocks it -> keep the socket+daemon (v1)."
echo "Log: tail /private/tmp/hpl1008-daemon.log   |   Cleanup: lpadmin -x HP_1008_be; sudo rm /usr/libexec/cups/backend/hpl100x"
