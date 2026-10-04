# Roadmap and status

The shipping driver (**v1.0**) is fully native and confirmed printing: patched SpliX
`rastertoqpdl` (C++ CUPS filter) plus `hpl1008-usbd` (C + IOKit USB), with a localhost
socket handing off between them. Everything below is "how much more native can it get."

A fundamental macOS constraint shapes all of it: **USB access needs root** on macOS 26
(non-root `libusb` enumerates 0 devices; `IOUSBInterfaceOpenSeize` needs root). So the USB
write always lives in a privileged, out-of-sandbox context. That is not a bug in this
project, it is the platform.

## V1 - Fully native driver  ✅ done (tagged v1.0)

SpliX encoder + native IOKit USB service. No Docker, VM, vendor binary, Python, or libusb.

## V2 - CUPS backend instead of the socket  ✅ confirmed working (2026-08-19)

`hpl1008-usbd.c` doubles as a CUPS backend (`hpl100x:/`) when invoked by that name, doing
the IOKit USB write directly. **Tested and it prints:** installed as a root-owned
(`0700`) backend at `/usr/libexec/cups/backend/hpl100x`, CUPS runs it as root and the
backend sandbox *does* permit IOKit USB matching, interface seize, and the bulk-OUT write.
So this route deletes both the localhost socket and the LaunchDaemon: the "USB needs root"
constraint is satisfied by CUPS running the root-owned backend, no separate daemon needed.

Two things the bring-up surfaced (both fixed):
- CUPS invokes a print-job backend with `argv[0]` set to the **device URI** (`hpl100x:/`),
  not the executable path. Detect backend mode from the whole `argv[0]` and the
  `DEVICE_URI` env var, not a basename match (the URI ends in `/`).
- The backend sandbox blocks writes to `/private/tmp`, so backend-mode logging goes to
  **stderr** (CUPS captures `DEBUG:`/`ERROR:`-prefixed lines into `error_log`).

A clean full A4 page printed through this path, so **the backend is now the default
install** (`install.sh` installs `hpl100x` and removes the socket + LaunchDaemon; the
daemon variant stays in `daemon/` + `launchd/` as a fallback). Bring-up also surfaced a
third fix that matters for both paths: the printer is **dual-mode** (interface 0 alt 0 =
`7/1/2` classic, alt 1 = `7/1/4` IPP-over-USB) and macOS leaves it on the IPP-USB alt,
where raw SPL3 is silently dropped; the driver now reads the config descriptor and
`SetAlternateInterface`s back to the classic alt before writing.

Reproduce with `experiments/test-v2-backend.sh` (or `-debug.sh` for the instrumented run).
Note: OpenPrinting considers the classic filter/backend model deprecated, so V4 (below) is
still the long-term direction.

## V2.1 - macOS's ippusbd steals the interface  ✅ fixed (2026-10-04)

A unit that had been printing stopped: every job failed with `seize failed 0xe00002c5
(interface held by macOS)` and CUPS paused the queue. Cause: macOS auto-starts
`/usr/libexec/ippusbd` for this printer (a per-device launchd job,
`com.apple.print.ippusb.<mfg>.<model>.<serial>`, submitted by `smd` on device enumeration)
and opens the USB interface **exclusively** to expose it as a driverless AirPrint device.
Our backend then can't seize it — in fact `USBDeviceOpenSeize` on the device itself returns
`kIOReturnExclusiveAccess`.

Things that did **not** work, and why:
- **Reclaim from inside the backend** (`SetConfiguration`, then `USBDeviceReEnumerate`): both
  need an open device handle, which the exclusive lock denies (`kIOReturnNotOpen`).
- **Kill `ippusbd` from inside the backend**: the CUPS backend sandbox hides other processes —
  `proc_listpids` returns no `ippusbd` — so the backend can't find or signal it. (The eviction
  code stays in `hpl1008-usbd.c`; it still works in the non-sandboxed one-shot/daemon modes and
  self-documents the attempt.)

What works: **disable the per-device ippusb launchd job** (`tools/disable-ippusb.sh`, run from
`install.sh`; `uninstall.sh --undo`). `launchctl disable` records the label in the persistent
disabled overrides, so the bridge stays gone across reboots and replug/wake, and the backend
seizes the free classic interface on the first attempt. We lose only macOS's driverless IPP-USB
for this printer — which is the garbled output this project exists to replace.

## V3 - Understand the protocol  ✅ core done

- `tools/spl3dump.py` parses any SPL3/QPDL-v3 stream (verified against HP and native output).
- `tools/SPL3-FORMAT.md` documents the field layout, including the 300-dpi-grid quirk.
- `tools/golden/` + `test_spl3dump.py` is a passing structural golden test.

Continuation: a standalone `libspl3` (extract the ~fraction of SpliX this family needs),
`raster2spl3`, and an `spl3verify` that decompresses `0x11` bands back to the raster and
checks they match. That turns the encoder into something fully owned and testable, and
lets HP's `rastertospl` be deleted even from the research environment.

## V4 - Printer Application (IPP front)  ⏳ designed, not built

The modern OpenPrinting replacement for classic drivers: present the printer outwardly as
a standard IPP device (PAPPL server), rasterize internally, feed `libspl3`, write over USB.
macOS then talks plain IPP and the SPL3 lives entirely below the app boundary.

Why it is the practical stopping point here:
- PAPPL is not brew-packaged; it (and its deps) must be built from source on macOS first.
- The full application is ~500-1000 lines (IPP capabilities, media, raster callback, USB).
- USB still needs root, so the PAPPL server runs privileged or uses a root USB helper; the
  clean user-space IPP abstraction that PAPPL gives on Linux is only partial on macOS.

Feasible and architecturally correct, but a large standalone project with marginal benefit
over the working v1.

## V5 - DriverKit .dext  ❌ hard wall

Replace the root IOKit daemon with a `USBDriverKit` system extension, packaged inside a
host app (`HP Laser 100x.app`). This is the "looks like something HP should have shipped"
end state. Blocked by prerequisites I cannot provision:
- DriverKit SDK is not in the Command Line Tools (needs full Xcode).
- No Apple Developer signing identity (0 present).
- The `com.apple.developer.driverkit` entitlement requires a paid Apple Developer account
  and Apple's explicit approval, and the `.dext` must be code-signed with a matching
  provisioning profile to load at all. macOS refuses unsigned/unentitled system extensions.

This is the genuine limit: it needs an Apple Developer account, Apple's approval, and code
signing, not just more code.
