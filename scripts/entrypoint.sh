#!/usr/bin/env bash
set -e

if ! command -v node >/dev/null 2>&1; then
  echo "[entrypoint] FATAL: node not found in PATH" >&2
  exit 1
fi

YEAR=$(date +%Y 2>/dev/null || echo 1970)
if [ "$YEAR" -lt 2024 ]; then
  echo "[entrypoint] WARNING: system clock may be wrong (year=$YEAR). SSL handshake can fail. Run: timedatectl"
fi

AA_PID=""
NODE_PID=""

cleanup() {
  [ -n "$AA_PID" ] && kill "$AA_PID" 2>/dev/null || true
  [ -n "$NODE_PID" ] && kill "$NODE_PID" 2>/dev/null || true
  exit 0
}
trap cleanup INT TERM

if [ "$AA_STUB" != "1" ] && command -v aa-handler >/dev/null 2>&1; then
  echo "[entrypoint] starting aa-handler"
  aa-handler "${CONFIG_PATH:-/config/config.yaml}" &
  AA_PID=$!
else
  echo "[entrypoint] AA_STUB=${AA_STUB:-0} — skipping aa-handler (server-side stub feeder will run if AA_STUB=1)"
fi

cd /app/server
export CONFIG_PATH="${CONFIG_PATH:-/config/config.yaml}"
export AA_STUB="${AA_STUB:-0}"

echo "[entrypoint] starting node server"
node dist/index.js &
NODE_PID=$!

# Exit as soon as ANY child dies, so Docker can restart cleanly
wait -n
EXIT=$?
echo "[entrypoint] child exited with code $EXIT — shutting down"
cleanup
exit "$EXIT"
