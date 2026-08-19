#!/usr/bin/env python3
"""spl3dump: parse an SPL3 / QPDL-v3 stream into a human-readable structure.

Documents the reverse-engineered format used by the HP Laser 1003-1008 (and the Samsung
M2020 / HP Laser 10x family). Field layout is taken from SpliX's qpdl.cpp and verified
against HP's own rastertospl output. Run it on any .spl/.prn stream:

    ./spl3dump.py job.spl

No printer needed. Part of understanding the protocol well enough to not need HP's binary.
"""
import sys, struct

PAPER = {0: "Letter", 1: "Legal", 2: "A4", 3: "Executive", 4: "Ledger", 5: "A3",
         6: "Env10", 7: "Monarch", 8: "C5", 9: "DL", 11: "B5", 16: "A5", 17: "A6"}
COMP = {0x0D: "0x0D", 0x0E: "0x0E", 0x11: "0x11 (LZ back-reference)",
        0x13: "0x13", 0x15: "0x15 (JBIG)"}
UEL = b"\x1b%-12345X"

def h(b):
    return " ".join(f"{x:02x}" for x in b)

def dump(data):
    m = data.find(b"ENTER LANGUAGE = QPDL")
    if m < 0:
        print("not a QPDL stream (no '@PJL ENTER LANGUAGE = QPDL')"); return
    pjl_end = data.find(b"\n", m) + 1

    print("== PJL job header ==")
    for chunk in data[:pjl_end].replace(UEL, b"").split(b"\r\n"):
        s = chunk.strip()
        if s.startswith(b"@PJL"):
            print("  " + s.decode(errors="replace"))

    uel = data.rfind(UEL)
    body = data[pjl_end:uel if uel > pjl_end else len(data)]
    print(f"\n== QPDL body ({len(body)} bytes) ==")

    i = 0
    page = 0
    bands = 0
    while i < len(body):
        rec = body[i]
        if rec == 0x00 and i + 0x11 <= len(body):                 # page header (signature byte 0)
            ph = body[i:i + 0x11]
            page += 1; bands = 0
            w = ph[5] << 8 | ph[6]; ht = ph[7] << 8 | ph[8]
            print(f"\nPAGE {page}")
            print(f"  resolution   : {ph[0x10]*100} x {ph[1]*100} dpi")
            print(f"  paper        : {PAPER.get(ph[4], ph[4])}")
            print(f"  page size    : {w} x {ht}  (300-dpi grid -> {w/300*25.4:.1f} x {ht/300*25.4:.1f} mm)")
            print(f"  copies       : {ph[2]<<8 | ph[3]}")
            print(f"  QPDL version : {ph[0xe]}")
            print(f"  raw header   : {h(ph)}")
            i += 0x11
        elif rec == 0x0C and i + 11 <= len(body):                 # band record
            band = body[i:i + 11]
            band_nr = band[1]; bw = band[2] << 8 | band[3]; bh = band[4] << 8 | band[5]
            comp = band[6]; ds = struct.unpack(">I", band[7:11])[0]
            sig = body[i + 11:i + 15]
            bands += 1
            print(f"  BAND {band_nr:>2}: {bw} x {bh}  comp={COMP.get(comp, hex(comp))}  "
                  f"payload={ds}B  sig={h(sig)}")
            i += 11 + ds
            if ds == 0:                                            # safety against a bad parse
                print("  (zero payload, stopping)"); break
        else:
            print(f"\n== end / trailer records ({len(body)-i} bytes) ==")
            print("  " + h(body[i:i + 24]) + (" ..." if len(body) - i > 24 else ""))
            break
    print(f"\n== UEL terminator ==  ({page} page(s) total)")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: spl3dump.py <spl-stream>", file=sys.stderr); sys.exit(2)
    dump(open(sys.argv[1], "rb").read())
