#!/bin/sh
# Quick IPC smoke test while aa-handler is running (Linux)
set -e
echo "Bridge status (connect server first)..."
timeout 2 nc -U /tmp/aa-bridge.sock </dev/null || true
echo "Video socket byte count (2s)..."
timeout 2 nc -U /tmp/aa-video.sock | wc -c
