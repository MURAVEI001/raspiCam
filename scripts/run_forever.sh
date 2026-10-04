#!/usr/bin/env bash
# Обёртка на случай, если systemd недоступен. Перезапускает бинарь при падении.
BIN=/usr/local/bin/mjpeg_streamer
CFG=${1:-/etc/mjpeg-streamer.ini}
while true; do
    "$BIN" --config "$CFG" || true
    echo "[run_forever] restarting in 1s..."
    sleep 1
done