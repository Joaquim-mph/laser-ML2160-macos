#!/usr/bin/env bash
# Disable macOS's IPP-over-USB bridge (ippusbd) for USB printers, so our classic raw backend
# can seize the printer's USB interface.
#
# Why this is needed: macOS auto-starts /usr/libexec/ippusbd for any IPP-USB-capable USB
# printer (HP Laser 100x advertises interface class 7 / protocol 4) and opens the interface
# *exclusively* to expose it as a driverless AirPrint device. That exclusive open blocks our
# backend: USBDeviceOpenSeize / USBInterfaceOpenSeize return kIOReturnExclusiveAccess
# (0xe00002c5), the job fails, and CUPS pauses the queue. Worse, the backend cannot pry it
# loose from the inside - the CUPS backend sandbox hides other processes (proc_listpids sees
# no ippusbd) and SetConfiguration/ReEnumerate need a device handle we cannot get. So the fix
# lives outside the backend: disable the per-device ippusb launchd job.
#
# The launchd label is generated per device and contains spaces:
#   com.apple.print.ippusb.<manufacturer>.<model>.<serial>
# `launchctl disable` records the label in the persistent disabled overrides, so it survives
# reboots and device replug/wake (smd re-submits the job on re-enumeration; launchd refuses a
# disabled label). We print over the printer's classic SPL3 interface, so this bridge is never
# needed for a printer driven by this project.
#
# Usage: tools/disable-ippusb.sh            disable + stop the ippusb bridge jobs
#        tools/disable-ippusb.sh --undo     re-enable them (restores macOS driverless IPP-USB)
# Re-runs itself under sudo (launchd system-domain changes need root).
set -euo pipefail

[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

undo=0
[ "${1:-}" = "--undo" ] && undo=1

# Labels currently known to launchd (running or merely loaded).
labels=$(launchctl list 2>/dev/null | sed -n 's/^.*\(com\.apple\.print\.ippusb\..*\)$/\1/p' || true)
# When undoing, also pick up labels that are only present in the disabled-overrides list.
if [ "$undo" = 1 ]; then
    disabled=$(launchctl print-disabled system 2>/dev/null \
        | sed -n 's/.*"\(com\.apple\.print\.ippusb\.[^"]*\)".*/\1/p' || true)
    labels=$(printf '%s\n%s\n' "$labels" "$disabled" | sort -u)
fi

if [ -z "${labels//[[:space:]]/}" ]; then
    echo "No com.apple.print.ippusb.* jobs found (nothing to $([ "$undo" = 1 ] && echo re-enable || echo disable))."
    exit 0
fi

printf '%s\n' "$labels" | while IFS= read -r label; do
    [ -n "$label" ] || continue
    if [ "$undo" = 1 ]; then
        launchctl enable "system/$label" 2>/dev/null || true
        echo "re-enabled: $label"
    else
        launchctl disable "system/$label" 2>/dev/null || true
        launchctl bootout  "system/$label" 2>/dev/null || true
        echo "disabled:   $label"
    fi
done
