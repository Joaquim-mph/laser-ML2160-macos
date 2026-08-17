# HP Laser 1008a → macOS (native Cmd‑P driver)

Make the **HP Laser 1003 / 1006 / 1008 (a/w)** — HP's rebadged Samsung SPL3 laser —
print from **Apple Silicon macOS** as a normal printer. `Cmd‑P` from any app, no
terminal, no per‑job scripts.

HP never shipped a working macOS driver for these. They're not AirPrint, they don't
speak PostScript/PCL, and the open‑source SPL2/QPDL drivers (splix, foo2zjs) get
*rejected* by the printer's SPL3 firmware. So this project runs **HP's own
`rastertospl`** — the exact codec, taken from HP's Unified Linux Driver — inside a tiny
Linux container, and delivers the result over USB itself.

> Tested on macOS 26 (Apple Silicon). USB‑only "a" models and USB‑connected "w" models.

---

## Why this is weird (and how it works)

The HP Laser 100 series is a genuinely awkward printer on a Mac:

| What you'd try | What happens |
| --- | --- |
| AirPrint / driverless | Not offered — the printer's USB HTTP endpoint serves no IPP |
| Generic PCL / PostScript | Printer speaks neither → CUPS backend hangs "offline" |
| splix (Samsung SPL2/QPDL) | Prints **garbage** — wrong SPL dialect |
| foo2zjs `foo2qpdl` | `SPL ERROR – Please use the proper driver` |
| HP's macOS driver | Doesn't exist |

The printer literally asks for "the proper driver." That driver exists — as an **x86 /
arm64 Linux binary** (`rastertospl`) in HP's Unified Linux Driver. It can't run on
macOS directly, but it runs *natively* in a Linux ARM64 container.

There's a second wall: even with correct SPL3, macOS's `usb` backend refuses to send to
this printer (it mis‑reads the USB port status as permanently "offline"), and on recent
macOS only **root** can talk to USB at all. And CUPS filters/backends run in a
**mandatory sandbox** that blocks both the container and USB. So the pipeline is split:

```mermaid
flowchart LR
    A[Any app · Cmd-P] --> B[CUPS queue<br/>HP_Laser_10x PPD]
    B -->|CUPS raster| C[socket backend<br/>127.0.0.1:9108]
    C --> D[hpl1008-daemon<br/>root · outside sandbox]
    D -->|raster| E[HP rastertospl<br/>in colima container]
    E -->|genuine SPL3| D
    D -->|libusb bulk write| F[(HP Laser 1008a)]
```

* The **CUPS queue** renders to CUPS raster and streams it to `socket://127.0.0.1:9108`
  using CUPS's own (sandbox‑allowed) `socket` backend.
* A **root LaunchDaemon** listens there, runs the raster through HP's `rastertospl` in
  the `hp-spl` container (→ real SPL3), and writes it straight to the printer's USB bulk
  endpoint with libusb — the two things the sandbox forbids, done outside it.

---

## Requirements

* Apple Silicon Mac (Intel works too; the installer picks the `x86_64` codec).
* [Homebrew](https://brew.sh).
* ~2 GB RAM for the background Linux VM (colima).
* The printer connected over **USB**.

## Install

```bash
git clone https://github.com/Kuberwastaken/hp-laser-1008a-macos.git
cd hp-laser-1008a-macos
./install.sh          # asks for your password once (installs the root helper)
```

The installer: installs `colima`/`docker`/`libusb`, starts the Linux VM, **downloads
HP's Unified Linux Driver** (not redistributed here), builds the codec container, and
sets up the printer queue + root daemon + login autostart.

Then just print to **HP Laser 1008a** from any app, or test:

```bash
lp -d HP_Laser_1008a /etc/hosts
```

## Uninstall

```bash
./uninstall.sh
```

---

## Notes & limitations

* **First print after idle is slow (~10‑15 s).** That's the *printer* waking from its
  aggressive auto‑power‑off and heating the fuser — not the software (the conversion +
  USB write take ~1 s). Back‑to‑back pages are quick.
* **colima must be running.** The installer adds a login‑item/LaunchAgent to start it.
  On **managed (Jamf) Macs** where `~/Library/LaunchAgents` is locked, the LaunchAgent
  won't load — add `colima start` as a **Login Item** (System Settings → General →
  Login Items) instead. After a reboot, the first print waits for colima to come up.
* **Different USB product id?** If `direct_write.py` says "printer not found" while
  `ioreg -p IOUSB -l | grep -iA2 "HP Laser"` shows the device, update `PID` in
  `~/.hp1008/direct_write.py` to the `idProduct` you see.
* **Logs:** `/private/tmp/hpl1008-daemon.log`.
* The installer relaxes the CUPS sandbox only if a previous version needed it; the
  current socket‑backend design does **not** require disabling the sandbox.

## Legal

This repo contains only glue code. It does **not** redistribute HP's driver — `install.sh`
downloads the Unified Linux Driver from HP at install time. `rastertospl` and the PPD are
HP's; using them to drive a printer you own is ordinary driver use.

## Credits

Built by reverse‑engineering the failure modes the printer itself reported. Thanks to the
[splix](https://github.com/OpenPrinting/splix) and [foo2zjs](https://github.com/koenkooi/foo2zjs)
projects (the SPL2/QPDL detour that proved the transport worked), Pierov's
[HP Laser 107a on Linux](https://www.pierov.org/2023/07/25/hp-laser-107a-linux/) writeup,
and HP's Unified Linux Driver for the actual SPL3 codec.

## License

MIT — see [LICENSE](LICENSE).
