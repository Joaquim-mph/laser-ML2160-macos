#!/bin/bash
# Fix "Filter failed" on Samsung ML-2160 queues that use Samsung's UPD driver.
#
# Samsung UPD 3.93's rastertosec crashes (atoi(NULL) in GeneratePJL) unless the
# PPD has a marked choice for Option4, MassStorage or FlashDrive. This adds a
# harmless Option4 ("Mass Storage", default Not Installed) to every ML-2160
# queue's PPD and reinstalls it. Safe to run more than once.
set -euo pipefail

queues=$(lpstat -v 2>/dev/null | grep -i 'ML-2160' | sed -E 's/^device for ([^:]+):.*/\1/')
if [ -z "$queues" ]; then
    echo "No ML-2160 printer found. Plug it in, add it in System Settings > Printers & Scanners, then rerun." >&2
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for q in $queues; do
    ppd=/etc/cups/ppd/$q.ppd
    if grep -q '^\*OpenUI \*Option4' "$ppd"; then
        echo "$q: already fixed"
        continue
    fi
    awk '
        /^\*CloseGroup: InstallableOptions/ && !done {
            print "*% Workaround: Samsung UPD rastertosec crashes unless Option4 is marked."
            print "*OpenUI *Option4/Mass Storage: Boolean"
            print "*OrderDependency: 10 AnySetup *Option4"
            print "*DefaultOption4: False"
            print "*Option4 False/Not Installed: \"\""
            print "*Option4 True/Installed: \"\""
            print "*CloseUI: *Option4"
            print ""
            done = 1
        }
        { print }
        END { if (!done) exit 1 }
    ' "$ppd" > "$tmp/$q.ppd" || { echo "$q: unexpected PPD layout, not changed" >&2; continue; }
    lpadmin -p "$q" -P "$tmp/$q.ppd" 2>/dev/null
    cupsenable "$q"
    echo "$q: fixed"
done

echo "Test with: lp -d <printer> /etc/hosts"
