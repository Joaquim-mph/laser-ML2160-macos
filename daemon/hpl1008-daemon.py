#!/usr/bin/python3
"""hpl1008-daemon: root LaunchDaemon that does the real printing OUTSIDE macOS's
CUPS sandbox.

The printer queue's device URI is socket://127.0.0.1:9108, so CUPS's own (sandbox
blessed) `socket` backend streams each CUPS-raster job to us. We convert it to genuine
SPL3 with HP's rastertospl (running in a Linux container via colima) and write it to
the printer over USB. Both steps are forbidden inside a CUPS filter/backend on macOS,
which is the whole reason this out-of-band daemon exists.

Paths below are filled in by install.sh (@@USERHOME@@ / @@DOCKER@@).
"""
import socket, subprocess, os, time

USERHOME = "@@USERHOME@@"
DOCKER   = "@@DOCKER@@"
PY       = f"{USERHOME}/.hp1008/venv/bin/python"
WRITER   = f"{USERHOME}/.hp1008/direct_write.py"
LOG      = "/private/tmp/hpl1008-daemon.log"
PORT     = 9108
JOB_TIMEOUT = 30        # safety net; the socket backend closes at end-of-job (EOF)
COLIMA_WAIT = 150       # max seconds to wait for the Linux VM to come up (post-reboot)
os.environ["DOCKER_HOST"] = f"unix://{USERHOME}/.colima/default/docker.sock"

def log(m):
    try: open(LOG, "a").write(f"{time.strftime('%F %T')} {m}\n")
    except Exception: pass

def docker_ready():
    try:
        return subprocess.run([DOCKER, "info"], capture_output=True).returncode == 0
    except Exception:
        return False

def wait_for_docker():
    """After a reboot the login item starts colima, which takes ~60s. Rather than
    erroring, hold the job until the VM is ready (or give up after COLIMA_WAIT)."""
    if docker_ready():
        return True
    log("colima/docker not ready; waiting for it to come up...")
    start = time.time()
    while time.time() - start < COLIMA_WAIT:
        time.sleep(3)
        if docker_ready():
            log(f"colima ready after {int(time.time() - start)}s")
            return True
    log("gave up waiting for colima")
    return False

def read_job(conn):
    """Read the whole job. End-of-job is the socket backend closing the connection
    (EOF); the timeout is only a safety net against a dead peer."""
    conn.settimeout(JOB_TIMEOUT)
    data = b""
    while True:
        try:
            chunk = conn.recv(1 << 16)
        except socket.timeout:
            break
        if not chunk:      # EOF => end of job
            break
        data += chunk
    return data

def handle(conn):
    data = read_job(conn)
    log(f"received {len(data)} raster bytes")
    if not data:
        return
    if not wait_for_docker():
        return
    try:
        conv = subprocess.run([DOCKER, "run", "-i", "--rm", "hp-spl"],
                              input=data, capture_output=True)
        if conv.returncode != 0:
            log("rastertospl failed: " + conv.stderr.decode(errors="replace")[:200]); return
        w = subprocess.run([PY, WRITER, "-"], input=conv.stdout, capture_output=True)
        if w.returncode == 0:
            log(f"printed ok ({len(conv.stdout)} SPL bytes)")
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
