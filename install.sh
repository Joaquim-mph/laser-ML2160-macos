#!/usr/bin/env bash
# Native macOS driver for the HP Laser 1003-1008 (a/w).
# Builds a patched SpliX `rastertoqpdl` (native SPL3/QPDL CUPS filter) and delivers the
# job over USB with a tiny root helper. No Docker, no Linux VM, no vendor binary.
# See README.md for how and why.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HP1008="$HOME/.hp1008"
BUILD="$HP1008/splix-build"
HP10X_PATCH_URL="https://github.com/OpenPrinting/splix/commit/206e283.patch"
PORT=9108

say(){ printf "\n\033[1;36m==> %s\033[0m\n" "$*"; }
die(){ printf "\033[1;31mError: %s\033[0m\n" "$*" >&2; exit 1; }

[ "$(uname)" = "Darwin" ] || die "macOS only."
command -v brew >/dev/null || die "Homebrew required: https://brew.sh"
BREW="$(brew --prefix)"
xcode-select -p >/dev/null 2>&1 || die "Xcode command line tools required: run 'xcode-select --install'"

say "Installing dependency (libusb)..."
brew list libusb >/dev/null 2>&1 || brew install libusb

say "Building the native SPL3 filter (patched SpliX)..."
rm -rf "$BUILD"
git clone --depth 1 https://github.com/photovirus/splix-macos.git "$BUILD"   # SpliX with macOS build fixes
cd "$BUILD"
# add HP Laser 10x support (upstream SpliX commit) + our 300-dpi header fix
curl -fsSL "$HP10X_PATCH_URL" -o /tmp/hp10x.patch || cp "$REPO_DIR/patches/hp-laser-10x-upstream-206e283.patch" /tmp/hp10x.patch
patch -p1 --forward --fuzz=3 < /tmp/hp10x.patch >/dev/null
patch -p1 --forward < "$REPO_DIR/patches/300dpi-header.patch" >/dev/null
make DISABLE_JBIG=1 >/dev/null
( cd ppd && sh compile.sh hp.drv.in -I . -d ./ >/dev/null )
[ -x optimized/rastertoqpdl ] && [ -f ppd/laser10x.ppd ] || die "SpliX build failed"

say "Setting up $HP1008..."
mkdir -p "$HP1008"
cp optimized/rastertoqpdl "$HP1008/rastertoqpdl"
cp ppd/laser10x.ppd       "$HP1008/laser10x.ppd"
/usr/bin/python3 -m venv "$HP1008/venv"
"$HP1008/venv/bin/pip" install -q --upgrade pip pyusb
sed "s#/opt/homebrew/lib/libusb-1.0.dylib#$BREW/lib/libusb-1.0.dylib#" \
    "$REPO_DIR/daemon/direct_write.py" > "$HP1008/direct_write.py"

say "Installing the native filter + root USB daemon (needs sudo)..."
sudo install -o root -g wheel -m 0755 "$HP1008/rastertoqpdl" /usr/libexec/cups/filter/rastertoqpdl
sed "s#@@USERHOME@@#$HOME#g" "$REPO_DIR/daemon/hpl1008-daemon.py" > /tmp/hpl1008-daemon
sudo install -o root -g wheel -m 0755 /tmp/hpl1008-daemon /usr/local/bin/hpl1008-daemon
sudo install -o root -g wheel -m 0644 "$REPO_DIR/launchd/com.hpl1008.daemon.plist" /Library/LaunchDaemons/com.hpl1008.daemon.plist
sudo launchctl bootout system/com.hpl1008.daemon 2>/dev/null || true
sudo launchctl bootstrap system /Library/LaunchDaemons/com.hpl1008.daemon.plist
sudo launchctl enable system/com.hpl1008.daemon

say "Creating the printer queue..."
lpadmin -p HP_Laser_1008a -E -v "socket://127.0.0.1:$PORT" -P "$HP1008/laser10x.ppd" \
        -o printer-is-shared=false -D "HP Laser 1008a" -L "USB (native SPL3)"
lpoptions -d HP_Laser_1008a >/dev/null

say "Done. Print to 'HP Laser 1008a' from any app (Cmd-P)."
echo "Quick test:  lp -d HP_Laser_1008a /etc/hosts"
echo "Logs:        /private/tmp/hpl1008-daemon.log"
