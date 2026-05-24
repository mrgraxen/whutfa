#!/usr/bin/env bash
# One-time host setup: install udev rules so the Linux kernel does not grab
# the phone in Google AOAP mode (18d1:2d00 / 2d01) before Docker/libusb can.
#
# Usage (from repository root):
#   sudo ./scripts/install-host-usb.sh
#   # or:
#   ./scripts/install-host-usb.sh   # re-invokes sudo automatically

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RULES_SRC="${ROOT}/config/99-android-auto-headunit.rules"
RULES_DEST="/etc/udev/rules.d/99-android-auto-headunit.rules"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "install-host-usb: Linux host only (Raspberry Pi or amd64 PC)."
  echo "On Windows/macOS use AA_STUB=1 for UI development without USB."
  exit 1
fi

if [[ "${EUID}" -ne 0 ]]; then
  echo "install-host-usb: root required — re-running with sudo..."
  exec sudo bash "$0" "$@"
fi

if [[ ! -f "${RULES_SRC}" ]]; then
  echo "install-host-usb: missing ${RULES_SRC}"
  exit 1
fi

echo "Installing udev rules from:"
echo "  ${RULES_SRC}"
install -m 644 "${RULES_SRC}" "${RULES_DEST}"
echo "  -> ${RULES_DEST}"

echo "Reloading udev..."
udevadm control --reload-rules
udevadm trigger
echo "udev rules active."

TARGET_USER="${SUDO_USER:-${USER}}"
if getent group plugdev >/dev/null 2>&1 && [[ -n "${TARGET_USER}" ]] && [[ "${TARGET_USER}" != "root" ]]; then
  if id -nG "${TARGET_USER}" 2>/dev/null | grep -qw plugdev; then
    echo "User ${TARGET_USER} is already in group plugdev."
  else
    echo "Adding ${TARGET_USER} to plugdev (optional; rules also set MODE=0666)..."
    usermod -aG plugdev "${TARGET_USER}" || true
    echo "  Log out and back in, or run: newgrp plugdev"
  fi
fi

echo ""
echo "Host USB setup complete."
echo "Next steps:"
echo "  docker compose up -d"
echo "  ./scripts/preflight-usb.sh"
echo "  docker exec headunit lsusb    # plug phone; expect 18d1:2d00 or 2d01"
