#!/usr/bin/python3
"""hpl1008-daemon: root LaunchDaemon, a pure QPDL-to-USB bridge.

The printer queue's device URI is socket://127.0.0.1:9108, so CUPS's own `socket`
backend streams each job here. The job is already genuine SPL3/QPDL, produced natively
by the patched SpliX `rastertoqpdl` CUPS filter (no Docker, no colima, no vendor binary).
We just write it to the printer's USB bulk endpoint.

This exists only because macOS forbids USB access inside a CUPS filter/backend (sandbox)
and only root can drive USB, so the write happens out here. Paths are filled by install.sh.
"""
import socket, subprocess, os, time

USERHOME = "@@USERHOME@@"
PY       = f"{USERHOME}/.hp1008/venv/bin/python"
WRITER   = f"{USERHOME}/.hp1008/direct_write.py"
LOG      = "/private/tmp/hpl1008-daemon.log"
PORT     = 9108
JOB_TIMEOUT = 30      # safety net; the socket backend closes at end-of-job (EOF)

def log(m):
    try: open(LOG, "a").write(f"{time.strftime('%F %T')} {m}\n")
    except Exception: pass

def read_job(conn):
    conn.settimeout(JOB_TIMEOUT)
    data = b""
    while True:
        try:
            chunk = conn.recv(1 << 16)
        except socket.timeout:
            break
        if not chunk:          # EOF => end of job
            break
        data += chunk
    return data

def handle(conn):
    data = read_job(conn)
    log(f"received {len(data)} QPDL bytes")
    if not data:
        return
    w = subprocess.run([PY, WRITER, "-"], input=data, capture_output=True)
    if w.returncode == 0:
        log("printed ok")
    else:
        log("USB write failed: " + (w.stdout + w.stderr).decode(errors="replace")[:200])

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", PORT)); s.listen(8)
    log(f"native daemon listening on 127.0.0.1:{PORT}")
    while True:
        conn, _ = s.accept()
        try: handle(conn)
        except Exception as e: log("handler error: " + str(e))
        finally:
            try: conn.close()
            except Exception: pass

if __name__ == "__main__":
    main()
