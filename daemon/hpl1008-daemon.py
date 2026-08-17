#!/usr/bin/python3
"""hpl1008-daemon — root LaunchDaemon that does the real printing OUTSIDE macOS's
CUPS sandbox.

The printer queue's device URI is socket://127.0.0.1:9108, so CUPS's own (sandbox-
blessed) `socket` backend streams each CUPS-raster job to us. We convert it to genuine
SPL3 with HP's rastertospl (running in a Linux container via colima) and write it to
the printer over USB. Both steps are forbidden inside a CUPS filter/backend on macOS,
which is the whole reason this out-of-band daemon exists.

Paths below are filled in by install.sh (@@USERHOME@@ / @@DOCKER@@).
"""
import socket, subprocess, os, time, tempfile

USERHOME = "@@USERHOME@@"
DOCKER   = "@@DOCKER@@"
PY       = f"{USERHOME}/.hp1008/venv/bin/python"
WRITER   = f"{USERHOME}/.hp1008/direct_write.py"
LOG      = "/private/tmp/hpl1008-daemon.log"
PORT     = 9108
IDLE_END = 2.0        # seconds of silence => the raster burst is complete
os.environ["DOCKER_HOST"] = f"unix://{USERHOME}/.colima/default/docker.sock"

def log(m):
    try: open(LOG, "a").write(f"{time.strftime('%F %T')} {m}\n")
    except Exception: pass

def read_job(conn):
    conn.settimeout(IDLE_END)
    data = b""
    while True:
        try:
            chunk = conn.recv(1 << 16)
        except socket.timeout:
            if data: break
            continue
        if not chunk: break
        data += chunk
    return data

def handle(conn):
    data = read_job(conn)
    log(f"received {len(data)} raster bytes")
    if not data:
        return
    try:
        conv = subprocess.run([DOCKER, "run", "-i", "--rm", "hp-spl"],
                              input=data, capture_output=True)
        if conv.returncode != 0:
            log("rastertospl failed: " + conv.stderr.decode(errors="replace")[:200]); return
        with tempfile.NamedTemporaryFile(suffix=".spl3", delete=False) as f:
            f.write(conv.stdout); spl = f.name
        w = subprocess.run([PY, WRITER, spl], capture_output=True)
        os.unlink(spl)
        if w.returncode == 0:
            log(f"printed ok ({len(conv.stdout)} SPL3 bytes)")
        else:
            log("USB write failed: " + (w.stdout + w.stderr).decode(errors="replace")[:200])
    except Exception as e:
        log("exception: " + str(e))

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", PORT)); s.listen(8)
    log(f"daemon listening on 127.0.0.1:{PORT}")
    while True:
        conn, _ = s.accept()
        try: handle(conn)
        except Exception as e: log("handler error: " + str(e))
        finally:
            try: conn.close()
            except Exception: pass

if __name__ == "__main__":
    main()
