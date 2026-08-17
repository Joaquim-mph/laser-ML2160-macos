#!/usr/bin/env python3
"""Write a raw SPL3 job to an HP Laser 1003-1008 over USB.

macOS's CUPS `usb` backend refuses to send to this printer (it mis-reads the USB
port status as permanently "offline"), so we bypass it and write straight to the
printer's classic bulk-OUT endpoint with libusb.

The USB layout is discovered from the device descriptors: HP vendor 0x03F0, the
classic printer-class interface (bInterfaceClass 7, bInterfaceProtocol 1/2, i.e.
NOT the protocol-4 IPP-over-USB interface), and its bulk-OUT endpoint. So other
product ids in the 1003-1008 family work without editing this file.

Must run as root: macOS attaches a kernel driver to the printer interface that
only root can detach, and on recent macOS only root can enumerate USB.

Usage:  direct_write.py <spl-file>      ("-" reads the job from stdin)
"""
import sys, time
import usb.core, usb.util, usb.backend.libusb1

HP_VID     = 0x03F0
KNOWN_PIDS = {0x069E}                              # HP Laser 1003-1008 (extend if needed)
LIBUSB     = "/opt/homebrew/lib/libusb-1.0.dylib"  # Apple Silicon Homebrew

def is_hp_laser(dev):
    if dev.idProduct in KNOWN_PIDS:
        return True
    try:
        name = usb.util.get_string(dev, dev.iProduct) or ""
    except Exception:
        name = ""
    return "laser" in name.lower()

def find_printer(be):
    for dev in usb.core.find(find_all=True, idVendor=HP_VID, backend=be):
        if is_hp_laser(dev):
            return dev
    return None

def find_bulk_out(dev):
    """Return (interface, alt, bulk-OUT endpoint) for the classic printer interface.
    Prefer bInterfaceProtocol 1/2 (raw printing); never the protocol-4 IPP-USB one."""
    try:
        cfg = dev.get_active_configuration()
    except usb.core.USBError:
        dev.set_configuration(1); cfg = dev.get_active_configuration()
    for intf in cfg:
        if intf.bInterfaceClass == 7 and intf.bInterfaceProtocol in (1, 2):
            for ep in intf:
                if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_OUT \
                   and usb.util.endpoint_type(ep.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK:
                    return intf.bInterfaceNumber, intf.bAlternateSetting, ep.bEndpointAddress
    return 0, 0, 0x02   # fall back to the known 1008a layout

def main(data):
    be = usb.backend.libusb1.get_backend(find_library=lambda _: LIBUSB)
    last = None
    for attempt in range(1, 7):
        dev = None
        try:
            dev = find_printer(be)
            if dev is None:
                print(f"[{attempt}] printer not found; waiting for it to wake..."); time.sleep(2); continue
            iface, alt, ep_out = find_bulk_out(dev)
            try:
                if dev.is_kernel_driver_active(iface):
                    dev.detach_kernel_driver(iface)
            except Exception as e:
                print("  detach note:", str(e)[:60])
            try: dev.get_active_configuration()
            except usb.core.USBError: dev.set_configuration(1)
            usb.util.claim_interface(dev, iface)
            try: dev.set_interface_altsetting(interface=iface, alternate_setting=alt)
            except Exception as e: print("  altset warn:", str(e)[:60])
            n = dev.write(ep_out, data, timeout=60000)
            print(f"wrote {n} bytes (intf {iface} alt {alt} ep 0x{ep_out:02x})")
            usb.util.release_interface(dev, iface)
            usb.util.dispose_resources(dev)
            return 0
        except usb.core.USBError as e:
            last = e
            print(f"[{attempt}] USB error: {str(e)[:70]}")
            try: usb.util.dispose_resources(dev)
            except Exception: pass
            time.sleep(1.5)
    print("FAILED after retries:", last, file=sys.stderr)
    return 1

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: direct_write.py <spl-file|->", file=sys.stderr); sys.exit(2)
    blob = sys.stdin.buffer.read() if sys.argv[1] == "-" else open(sys.argv[1], "rb").read()
    sys.exit(main(blob))
