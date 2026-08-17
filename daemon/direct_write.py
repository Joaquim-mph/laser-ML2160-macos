#!/usr/bin/env python3
"""Write a raw SPL3 job to the HP Laser 1003-1008 over USB.

The printer exposes a classic USB printer interface (interface 0, alt 0,
bInterfaceProtocol=2) with a bulk-OUT endpoint at 0x02. macOS's CUPS `usb` backend
refuses to send to it (it mis-reads the port status as "offline"), so we bypass it
and write straight to the endpoint with libusb.

Must run as root: macOS attaches a kernel driver to interface 0 that only root can
detach, and on recent macOS only root can enumerate USB at all. (The daemon runs as
root, so this is automatic.)

If your unit enumerates with a different USB product id, adjust PID below
(`ioreg -p IOUSB -l | grep -i "HP Laser" -A2` shows idVendor/idProduct).
"""
import sys, time
import usb.core, usb.util, usb.backend.libusb1

VID, PID = 0x03F0, 0x069E                    # HP: 0x03F0 ; 1003-1008: 0x069E
INTERFACE, ALT, EP_OUT = 0, 0, 0x02
LIBUSB = "/opt/homebrew/lib/libusb-1.0.dylib"   # Apple Silicon Homebrew

def main(path):
    data = open(path, "rb").read()
    be = usb.backend.libusb1.get_backend(find_library=lambda _: LIBUSB)
    last = None
    for attempt in range(1, 7):
        dev = None
        try:
            dev = usb.core.find(idVendor=VID, idProduct=PID, backend=be)
            if dev is None:
                print(f"[{attempt}] printer not found; waiting for it to wake…")
                time.sleep(2); continue
            try:
                if dev.is_kernel_driver_active(INTERFACE):
                    dev.detach_kernel_driver(INTERFACE)
            except Exception as e:
                print("  detach note:", str(e)[:60])
            try:
                dev.get_active_configuration()
            except usb.core.USBError:
                dev.set_configuration(1)
            usb.util.claim_interface(dev, INTERFACE)
            try:
                dev.set_interface_altsetting(interface=INTERFACE, alternate_setting=ALT)
            except Exception as e:
                print("  altset warn:", str(e)[:60])
            n = dev.write(EP_OUT, data, timeout=60000)
            print(f"wrote {n} bytes to the printer")
            usb.util.release_interface(dev, INTERFACE)
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
        print("usage: direct_write.py <spl3-file>", file=sys.stderr); sys.exit(2)
    sys.exit(main(sys.argv[1]))
