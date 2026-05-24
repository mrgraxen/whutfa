#!/bin/sh
# Quick USB checklist before plugging in a phone (run on Linux host).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RULES_INSTALLED="/etc/udev/rules.d/99-android-auto-headunit.rules"

echo "=== USB preflight ==="
echo ""

if [ "$(uname -s)" != "Linux" ]; then
  echo "Linux host required for real USB. Use AA_STUB=1 elsewhere."
  exit 0
fi

if [ -f "$RULES_INSTALLED" ]; then
  echo "[ok] udev rules installed: $RULES_INSTALLED"
else
  echo "[!!] udev rules NOT installed."
  echo "     Run: sudo ${ROOT}/scripts/install-host-usb.sh"
  echo ""
fi

if command -v lsusb >/dev/null 2>&1; then
  echo "Current USB devices:"
  lsusb
else
  echo "lsusb not found (install usbutils)."
fi

echo ""
echo "After plugging in the phone you should see:"
echo "  1) Phone vendor VID/PID"
echo "  2) Brief disconnect"
echo "  3) Google 18d1:2d00 or 18d1:2d01"
echo ""
echo "Inside Docker:"
echo "  docker exec headunit lsusb"
echo "  docker logs headunit 2>&1 | tail -30"
