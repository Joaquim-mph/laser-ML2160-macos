#!/usr/bin/env bash
# Native macOS driver installer for the HP Laser 1003-1008 (a/w).
# Downloads HP's Unified Linux Driver, runs its real `rastertospl` (SPL3) in a small
# Linux container via colima, and delivers the job over USB with a root helper daemon
# so any app can just Cmd-P.  See README.md for how/why.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ULD_URL="${HPL1008_ULD_URL:-https://ftp.hp.com/pub/softlib/software13/printers/MFP170/uld-hp_V1.00.39.12_00.15.tar.gz}"
HP1008="$HOME/.hp1008"
PORT=9108

say(){ printf "\n\033[1;36m==> %s\033[0m\n" "$*"; }
die(){ printf "\033[1;31mError: %s\033[0m\n" "$*" >&2; exit 1; }

# 0. sanity ------------------------------------------------------------------
[ "$(uname)" = "Darwin" ] || die "macOS only."
case "$(uname -m)" in
  arm64)  ULD_ARCH=aarch64; PLATFORM=linux/arm64 ;;
  x86_64) ULD_ARCH=x86_64;  PLATFORM=linux/amd64 ;;
  *) die "unsupported arch $(uname -m)";;
esac
command -v brew >/dev/null || die "Homebrew required - https://brew.sh"
BREW="$(brew --prefix)"

# 1. deps + Linux VM ---------------------------------------------------------
say "Installing dependencies (colima, docker, libusb)…"
for f in colima docker libusb; do brew list "$f" >/dev/null 2>&1 || brew install "$f"; done
DOCKER="$(command -v docker)"; COLIMA="$(command -v colima)"

say "Starting colima…"
colima status >/dev/null 2>&1 || colima start --cpu 2 --memory 2
docker info >/dev/null 2>&1 || die "docker not reachable after 'colima start'."

# 2. fetch HP's driver (NOT redistributed here) ------------------------------
say "Downloading HP Unified Linux Driver (contains rastertospl)…"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
curl -fL --retry 3 -o "$TMP/uld.tgz" "$ULD_URL" || die "ULD download failed (URL may have moved - see README)."
tar xzf "$TMP/uld.tgz" -C "$TMP"
[ -x "$TMP/uld/$ULD_ARCH/rastertospl" ] || die "rastertospl ($ULD_ARCH) missing from ULD."

# 3. ~/.hp1008: python writer + PPD ------------------------------------------
say "Setting up $HP1008…"
mkdir -p "$HP1008"
/usr/bin/python3 -m venv "$HP1008/venv"
"$HP1008/venv/bin/pip" install -q --upgrade pip pyusb
sed "s#/opt/homebrew/lib/libusb-1.0.dylib#$BREW/lib/libusb-1.0.dylib#" \
    "$REPO_DIR/daemon/direct_write.py" > "$HP1008/direct_write.py"
cp "$TMP/uld/noarch/share/ppd/HP_Laser_10x_Series.ppd" "$HP1008/10x.ppd"

# 4. build the codec image ---------------------------------------------------
say "Building the hp-spl container image (bakes in HP's codec)…"
CTX="$TMP/ctx"; mkdir -p "$CTX/uld"
cp "$TMP/uld/$ULD_ARCH/rastertospl" "$CTX/uld/"
cp "$TMP/uld/$ULD_ARCH/"*.so "$CTX/uld/" 2>/dev/null || true
cp "$HP1008/10x.ppd" "$CTX/10x.ppd"
cp "$REPO_DIR/docker/Dockerfile" "$REPO_DIR/docker/run.sh" "$CTX/"
docker build --platform "$PLATFORM" -t hp-spl "$CTX" >/dev/null
echo "  built hp-spl"

# 5. queue PPD (no-op filter so the job reaches the socket backend as raster) -
sed 's#application/vnd.cups-raster 0 rastertospl#application/vnd.cups-raster 0 hpl1008-raster#' \
    "$HP1008/10x.ppd" > "$HP1008/hpl1008.ppd"

# 6. root daemon + filter + LaunchDaemon (sudo) ------------------------------
say "Installing the root print helper (needs sudo)…"
sed -e "s#@@USERHOME@@#$HOME#g" -e "s#@@DOCKER@@#$DOCKER#g" \
    "$REPO_DIR/daemon/hpl1008-daemon.py" > "$TMP/hpl1008-daemon"
sudo install -o root -g wheel -m 0755 "$TMP/hpl1008-daemon" /usr/local/bin/hpl1008-daemon
sudo install -o root -g wheel -m 0755 "$REPO_DIR/cups/hpl1008-raster" /usr/libexec/cups/filter/hpl1008-raster
sudo install -o root -g wheel -m 0644 "$REPO_DIR/launchd/com.hpl1008.daemon.plist" /Library/LaunchDaemons/com.hpl1008.daemon.plist
sudo launchctl bootout system/com.hpl1008.daemon 2>/dev/null || true
sudo launchctl bootstrap system /Library/LaunchDaemons/com.hpl1008.daemon.plist
sudo launchctl enable system/com.hpl1008.daemon

# 7. colima autostart at login (best effort) ---------------------------------
say "Enabling colima autostart at login…"
if mkdir -p "$HOME/Library/LaunchAgents" 2>/dev/null && \
   sed "s#@@COLIMA@@#$COLIMA#g" "$REPO_DIR/launchd/com.hpl1008.colima.plist" \
     > "$HOME/Library/LaunchAgents/com.hpl1008.colima.plist" 2>/dev/null; then
  launchctl bootout "gui/$(id -u)/com.hpl1008.colima" 2>/dev/null || true
  launchctl bootstrap "gui/$(id -u)" "$HOME/Library/LaunchAgents/com.hpl1008.colima.plist" 2>/dev/null \
    && echo "  LaunchAgent installed" \
    || echo "  couldn't load LaunchAgent - add 'colima start' as a Login Item (see README)."
else
  echo "  ~/Library/LaunchAgents not writable (managed Mac?) - add 'colima start' as a Login Item (see README)."
fi

# 8. create the queue --------------------------------------------------------
say "Creating printer queue 'HP_Laser_1008a'…"
lpadmin -p HP_Laser_1008a -E -v "socket://127.0.0.1:$PORT" -P "$HP1008/hpl1008.ppd" \
        -o printer-is-shared=false -D "HP Laser 1008a" -L "USB via SPL3 helper"
lpoptions -d HP_Laser_1008a >/dev/null

say "Done. Print to 'HP Laser 1008a' from any app (Cmd-P)."
echo "Quick test:  lp -d HP_Laser_1008a /etc/hosts"
echo "Logs:        /private/tmp/hpl1008-daemon.log"
