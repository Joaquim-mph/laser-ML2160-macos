#!/bin/bash
# V2 diagnostic re-run: the plain test disabled the queue and printed nothing, and the
# backend's own /tmp log was sandbox-blocked. This routes backend diagnostics to stderr
# (which CUPS captures into error_log) and turns on debug logging, so we see EXACTLY where
# the backend dies: process launch, USB device match, interface seize, or the WritePipe.
set -e
H="$HOME/.hp1008"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> enabling CUPS debug logging"
sudo cupsctl --debug-logging

echo "==> reinstalling the instrumented backend"
sudo install -o root -g wheel -m 0700 "$H/hpl1008-usbd" /usr/libexec/cups/backend/hpl100x

echo "==> re-enabling the queue (last run disabled it) + retry policy"
sudo cupsenable HP_1008_be 2>/dev/null || true
lpadmin -p HP_1008_be -o printer-error-policy=abort-job 2>/dev/null || true

echo "==> clearing old error_log marker + test print"
BEFORE=$(sudo wc -l < /var/log/cups/error_log)
lp -d HP_1008_be "$HERE/testpage.txt"
sleep 4

echo
echo "==> new error_log lines (hpl100x / backend / this job):"
sudo tail -n +"$((BEFORE+1))" /var/log/cups/error_log | grep -iE "hpl100x|backend|1008_be|WritePipe|seize|iface|found device|printed|sandbox|deny|Fail" || \
  sudo tail -25 /var/log/cups/error_log

echo
echo "==> queue state:"; lpstat -p HP_1008_be
echo "(turn debug logging back off later with: sudo cupsctl --no-debug-logging)"
