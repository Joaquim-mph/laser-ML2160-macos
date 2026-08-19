#!/usr/bin/env python3
"""Golden test: spl3dump must parse the known native A4 fixture correctly."""
import subprocess, sys, os
here = os.path.dirname(os.path.abspath(__file__))
out = subprocess.run([sys.executable, os.path.join(here, "spl3dump.py"),
                      os.path.join(here, "golden", "mini_a4.spl")],
                     capture_output=True, text=True).stdout
expect = ["paper        : A4", "page size    : 2480", "QPDL version : 3",
          "comp=0x11", "4864 x 128", "1 page(s) total"]
missing = [e for e in expect if e not in out]
if missing:
    print("FAIL, missing:", missing); print(out); sys.exit(1)
print("golden test OK")
