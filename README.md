# HP Laser 1008a on macOS (native Cmd-P driver)

Make the **HP Laser 1003 / 1006 / 1008 (a/w)**, HP's rebadged Samsung SPL3 laser, print
from **Apple Silicon macOS** like a normal printer. `Cmd-P` from any app. No terminal,
no per-job scripts.

HP never shipped a working macOS driver for these. They are not AirPrint, they do not
speak PostScript or PCL, and until recently the open-source SPL/QPDL drivers did not
produce a stream this exact unit accepts. This project fixes that with a small patch to
**SpliX** and drives the printer natively.

> Tested on macOS 26 (Apple Silicon). USB-connected 1003 / 1006 / 1008 (a and w).

## UPDATE: this driver is now fully native (and that is what we run)

It got progressively more native, and the current version is the real deal:

1. **First** it ran HP's proprietary Linux `rastertospl` inside a Docker/colima Linux VM
   (the only thing that produced correct output at the time).
2. **Then** we reverse engineered the actual bug (see [The story](#the-story)), fixed it
   in SpliX with a 10-line patch, and dropped Docker/colima/ULD entirely.
3. **Now** the USB write is a tiny native **IOKit** helper too, so **Python, pyusb, and
   libusb are gone as well**.

**This is what the repo ships and what we use.** The entire runtime is two compiled
pieces on Apple's own frameworks: a patched SpliX `rastertoqpdl` CUPS filter (C++, system
`libcups`) and `hpl1008-usbd` (C, `IOKit` + `CoreFoundation`). No Docker, no VM, no vendor
binary, no Python, no Homebrew libraries. The SPL3 fix is being upstreamed to SpliX
([issue #1](https://github.com/Kuberwastaken/hp-laser-1008a-macos/issues/1)).

## Install (one command)

Plug the printer in over USB, then paste this into Terminal:

```bash
git clone https://github.com/Kuberwastaken/hp-laser-1008a-macos.git && cd hp-laser-1008a-macos && ./install.sh
```

It asks for your password once, builds the two native binaries, sets up the printer as
**"HP Laser 1008a"**, and disables macOS's conflicting IPP-over-USB bridge for it (see
[How it works](#how-it-works)). Print from any app with `Cmd-P`. Prerequisites:
[Homebrew](https://brew.sh) and the Xcode command line tools (`xcode-select --install`),
both build-time only.

Test from the terminal: `lp -d HP_Laser_1008a /etc/hosts`. Remove everything: `./uninstall.sh`.

## How it works

```mermaid
flowchart LR
    A[Any app, Cmd-P] --> B[CUPS]
    B --> C[cgpdftoraster<br/>CUPS raster]
    C --> D[rastertoqpdl<br/>patched SpliX, C++]
    D -->|SPL3 / QPDL| E[hpl100x backend<br/>C + IOKit, root]
    E -->|USB bulk write| F[(HP Laser 1008a)]
```

Two non-obvious pieces make this work:

**The backend runs as root.** On recent macOS only **root** can drive USB, and the `usb`
backend that ships with macOS refuses this printer (it mis-reads the port status as
permanently "offline"). So the USB write lives in a small CUPS **backend** (`hpl100x:/`)
installed `0700` root-owned: CUPS runs it as root and it does the raw write with `IOKit`
(`IOUSBInterfaceOpenSeize` + `WritePipe`). The rest of the chain is a normal native CUPS
filter. (The backend sandbox does permit IOKit USB; only the *filter* sandbox blocks it.)

**The printer is dual-mode and macOS flips it.** Interface 0 has alt 0 = `7/1/2` (classic
raw printing) and alt 1 = `7/1/4` (IPP-over-USB), sharing bulk-OUT endpoint `0x02`. macOS
often leaves the interface on the IPP-USB alt, where the printer expects HTTP framing and
silently drops raw SPL3. The backend reads the config descriptor, finds the classic
interface/alt, and `SetAlternateInterface`s back to it before writing. Run
`sudo /usr/libexec/cups/backend/hpl100x probe` to dump the descriptor.

**macOS grabs the interface first (`ippusbd`).** Because the printer advertises the
IPP-over-USB alt, macOS auto-starts `/usr/libexec/ippusbd` for it (a per-device launchd job,
`com.apple.print.ippusb.<mfg>.<model>.<serial>`) and opens the USB interface *exclusively* to
expose it as a driverless AirPrint device. Our backend then loses the seize with
`kIOReturnExclusiveAccess` (`0xe00002c5`) — on the device open itself — the job fails and CUPS
pauses the queue. The backend can't recover from inside: the CUPS backend sandbox hides other
processes (it can't find, let alone kill, `ippusbd`), and `SetConfiguration`/`USBDeviceReEnumerate`
need a device handle the exclusive lock denies. So `install.sh` disables that per-device bridge
(`tools/disable-ippusb.sh`, via `launchctl disable`), which persists across reboots and
replug/wake; `uninstall.sh --undo`s it. With the bridge gone the interface stays free and the
backend seizes it on the first try. (We lose nothing: this printer's driverless IPP-USB output
is the garbled path this project exists to replace.)

> Earlier versions streamed SPL3 to a root LaunchDaemon over `127.0.0.1:9108` because the
> backend sandbox was assumed to block USB. It does not, so the socket and daemon are gone
> (see `ROADMAP.md`, V2). The daemon variant still lives in `daemon/` + `launchd/` as a
> fallback.

## The story

The HP Laser 100 series is a genuinely awkward printer on a Mac:

| What you'd try | What happens |
| --- | --- |
| AirPrint / driverless | Not offered. Its USB HTTP endpoint serves no IPP |
| Generic PCL / PostScript | Printer speaks neither, the CUPS backend hangs "offline" |
| SpliX 2.0.1 | No HP Laser 10x support at all |
| SpliX 2.0.2 (out of the box) | Garbled: striped raster at the page origin, repeated sheets |
| HP's macOS driver | Does not exist |

SpliX 2.0.2 added HP Laser 10x support (QPDL v3, a band-width table), but on the 1008a it
still printed a striped patch at the top-left of every sheet and then ejected and
repeated. We diagnosed it without the printer by feeding an identical raster through both
HP's `rastertospl` and SpliX and diffing the output:

- The band records were **identical** (4864x128, compression 0x11), differing only in the
  lossless compressed bytes, so the printer decodes them the same. A red herring.
- The one real difference was the **page-header geometry unit**: HP emits `2480 x 3507`
  (a 300-dpi grid), SpliX emitted `4960 x 6912` (600-dpi). This printer reads the header
  size on a 300-dpi grid, so SpliX's value looked like a ~16 x 23 inch page. It laid one
  band at the top, hit the real A4 edge, ejected, believed a giant page remained, and
  repeated. Exactly the symptom.

HP's own binary confirmed it: disassembling `rastertospl` shows it computes the header as
`points * 300.0 / 72.0`. SpliX already had that 300-dpi code; it was just wrongly coupled
to JBIG. The fix (`patches/300dpi-header.patch`) decouples it and gates it on
`specialBandWidth`, which is already true for these models. Bands stay 0x11. Ten lines.

## Notes

* **First print after idle is slow (~10-15s).** That is the printer waking from its
  aggressive auto-power-off and heating the fuser, not the software.
* **Different USB product id?** `hpl1008-usbd` matches HP vendor `0x03F0` and the classic
  printer-class interface by descriptor, so other 1003/1008 PIDs work as-is.
* **Nothing prints?** Turn on CUPS debug logging (`sudo cupsctl --debug-logging`) and watch
  `tail -f /var/log/cups/error_log` for the backend's `hpl100x:` lines.
  * `seize failed 0x…02c5 (interface held by macOS)` means macOS's `ippusbd` re-grabbed the
    interface (e.g. after an OS update re-enabled the bridge). Re-run `sudo tools/disable-ippusb.sh`
    then `cupsenable HP_Laser_1008a`.
  * `no classic bulk-out interface` means the printer re-enumerated in IPP-USB mode; the
    alt-setting switch handles it, and `sudo /usr/libexec/cups/backend/hpl100x probe` shows
    the current descriptor.
  * A queue stuck `disabled`/paused after a past failure just needs `cupsenable HP_Laser_1008a`.
* **Logs:** the backend logs to CUPS `error_log` (via stderr); the legacy daemon logs to
  `/private/tmp/hpl1008-daemon.log`.

## Credits

Thanks to [SpliX](https://github.com/OpenPrinting/splix) (the SPL/QPDL engine), @ValdikSS
for the HP Laser 10x support in SpliX 2.0.2, @janrueth / photovirus for the macOS build
patch, and Pierov's [HP Laser 107a on Linux](https://www.pierov.org/2023/07/25/hp-laser-107a-linux/)
writeup. The `300dpi-header.patch` here is the missing piece for the 1003/1008 series.

## License

MIT for this glue (see [LICENSE](LICENSE)). SpliX and the patches are GPLv2.
