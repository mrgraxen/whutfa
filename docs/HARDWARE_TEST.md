# Hardware test checklist (Raspberry Pi + phone)

Run on **Linux host** with USB passthrough to Docker. Not automatable in CI.

## Prerequisites

- [ ] Raspberry Pi 4/5 (or Linux amd64 PC), 64-bit OS
- [ ] Data-capable USB cable (not charge-only)
- [ ] Host udev rules installed: `sudo ./scripts/install-host-usb.sh`
- [ ] Preflight OK: `./scripts/preflight-usb.sh`
- [ ] Android Auto developer settings: Unknown sources enabled
- [ ] System clock synced (`timedatectl`)

## Phase 1 — USB / AOA

```bash
docker compose up -d
docker exec whutfa lsusb
# Plug phone
docker exec whutfa lsusb   # expect 18d1:2d00 or 2d01
docker logs whutfa 2>&1 | tail -50
```

- [ ] Two-step enumeration observed
- [ ] Log shows AOAP open (not only pre-AOA VID)

## Phase 2 — AA session

- [ ] `aa-handler` logs NAL unit sizes > 0 for 10+ seconds
- [ ] No `SSL_HANDSHAKE_FAILED` in logs

## Phase 3 — IPC

```bash
# While session active
nc -U /tmp/aa-video.sock | wc -c   # expect non-zero (on host if socket mounted)
```

## Phase 5–7 — Browser E2E

- [ ] Open `http://<pi-ip>:8080` in Chromium
- [ ] Video visible on canvas
- [ ] Audio audible
- [ ] Touch moves focus on phone UI

Record results in [COMPATIBILITY.md](COMPATIBILITY.md).
