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

It asks for your password once, builds the two native binaries, and sets up the printer
as **"HP Laser 1008a"**. Print from any app with `Cmd-P`. Prerequisites: [Homebrew](https://brew.sh)
and the Xcode command line tools (`xcode-select --install`), both build-time only.

Test from the terminal: `lp -d HP_Laser_1008a /etc/hosts`. Remove everything: `./uninstall.sh`.

## How it works

```mermaid
flowchart LR
    A[Any app, Cmd-P] --> B[CUPS]
    B --> C[cgpdftoraster<br/>CUPS raster]
    C --> D[rastertoqpdl<br/>patched SpliX, C++]
    D -->|SPL3 / QPDL| E[socket 127.0.0.1:9108]
    E --> F[hpl1008-usbd<br/>C + IOKit, root]
    F -->|USB bulk write| G[(HP Laser 1008a)]
```

The one non-obvious piece is the daemon. macOS forbids USB access inside a CUPS
filter/backend (a hardened sandbox), and on recent macOS only **root** can drive USB at
all. Its own `usb` backend also refuses this printer (it mis-reads the port status as
permanently "offline"). So the queue streams the finished SPL3 to a small root
LaunchDaemon over localhost, and that daemon does the raw USB write with `IOKit`
(`IOUSBInterfaceOpenSeize` + `WritePipe`). Everything upstream of it is a normal native
CUPS filter.

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
* **Logs:** `/private/tmp/hpl1008-daemon.log`.

## Credits

Thanks to [SpliX](https://github.com/OpenPrinting/splix) (the SPL/QPDL engine), @ValdikSS
for the HP Laser 10x support in SpliX 2.0.2, @janrueth / photovirus for the macOS build
patch, and Pierov's [HP Laser 107a on Linux](https://www.pierov.org/2023/07/25/hp-laser-107a-linux/)
writeup. The `300dpi-header.patch` here is the missing piece for the 1003/1008 series.

## License

MIT for this glue (see [LICENSE](LICENSE)). SpliX and the patches are GPLv2.
