#!/usr/bin/env bash
# Native macOS driver for the HP Laser 1003-1008 (a/w).
# Builds a patched SpliX `rastertoqpdl` (native SPL3/QPDL CUPS filter) and a tiny native
# IOKit USB helper. No Docker, no Linux VM, no vendor binary, no Python, no libusb.
# Just Apple system frameworks and an open-source SPL3 fix. See README.md.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HP1008="$HOME/.hp1008"
BUILD="$HP1008/splix-build"
HP10X_PATCH_URL="https://github.com/OpenPrinting/splix/commit/206e283.patch"

say(){ printf "\n\033[1;36m==> %s\033[0m\n" "$*"; }
die(){ printf "\033[1;31mError: %s\033[0m\n" "$*" >&2; exit 1; }

[ "$(uname)" = "Darwin" ] || die "macOS only."
command -v brew >/dev/null || die "Homebrew required: https://brew.sh"
xcode-select -p >/dev/null 2>&1 || die "Xcode command line tools required: run 'xcode-select --install'"

say "Building the native SPL3 filter (patched SpliX)..."
mkdir -p "$HP1008"
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
cp optimized/rastertoqpdl "$HP1008/rastertoqpdl"
cp ppd/laser10x.ppd       "$HP1008/laser10x.ppd"

say "Compiling the native IOKit USB helper..."
clang -O2 -o "$HP1008/hpl1008-usbd" "$REPO_DIR/daemon/hpl1008-usbd.c" \
    -framework IOKit -framework CoreFoundation -Wno-deprecated-declarations
otool -L "$HP1008/hpl1008-usbd" | grep -qi "libusb\|Python" && die "helper picked up an unexpected dependency"

# Remove any earlier socket+daemon install (this driver used to hand off over 127.0.0.1).
# The binary is now a CUPS backend, so the daemon and its localhost socket are not needed.
sudo launchctl bootout system/com.hpl1008.daemon 2>/dev/null || true
sudo rm -f /Library/LaunchDaemons/com.hpl1008.daemon.plist /usr/local/bin/hpl1008-daemon 2>/dev/null || true

say "Installing the native filter + IOKit USB backend (needs sudo)..."
# The same binary is a CUPS *backend* when invoked as 'hpl100x'. Installed 0700 root-owned,
# CUPS runs it as root (required to seize the USB interface) and it does the bulk-OUT write
# directly. No socket, no LaunchDaemon. The backend forces the printer's classic alt-setting
# (it is dual-mode: classic 7/1/2 raw printing vs 7/1/4 IPP-over-USB) before writing.
sudo install -o root -g wheel -m 0755 "$HP1008/rastertoqpdl"  /usr/libexec/cups/filter/rastertoqpdl
sudo install -o root -g wheel -m 0700 "$HP1008/hpl1008-usbd"  /usr/libexec/cups/backend/hpl100x

say "Creating the printer queue..."
lpadmin -p HP_Laser_1008a -E -v "hpl100x:/" -P "$HP1008/laser10x.ppd" \
        -o printer-is-shared=false -D "HP Laser 1008a" -L "USB (native SPL3)"
lpoptions -d HP_Laser_1008a >/dev/null

say "Done. Print to 'HP Laser 1008a' from any app (Cmd-P)."
echo "Quick test:  lp -d HP_Laser_1008a /etc/hosts"
echo "USB map:     sudo /usr/libexec/cups/backend/hpl100x probe   (dumps the device descriptor)"
echo "Debug logs:  sudo cupsctl --debug-logging   then   tail /var/log/cups/error_log"
