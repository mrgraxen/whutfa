#!/bin/sh
set -e

YEAR=$(date +%Y 2>/dev/null || echo 1970)
if [ "$YEAR" -lt 2024 ]; then
  echo "[entrypoint] WARNING: system clock may be wrong (year=$YEAR). SSL handshake can fail. Run: timedatectl"
fi

if [ "$AA_STUB" != "1" ]; then
  echo "[entrypoint] starting aa-handler"
  aa-handler "$CONFIG_PATH" &
  AA_PID=$!
else
  echo "[entrypoint] AA_STUB=1 — skipping aa-handler (server-side stub feeder)"
  AA_PID=""
fi

cleanup() {
  [ -n "$AA_PID" ] && kill "$AA_PID" 2>/dev/null || true
  kill "$NODE_PID" 2>/dev/null || true
  exit 0
}
trap cleanup INT TERM

cd /app/server
export CONFIG_PATH="${CONFIG_PATH:-/config/config.yaml}"
export AA_STUB="${AA_STUB:-0}"
node dist/index.js &
NODE_PID=$!

wait "$NODE_PID"
