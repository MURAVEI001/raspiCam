#!/usr/bin/env bash
set -euo pipefail

# Установка зависимостей и сборка
sudo apt-get update
sudo apt-get install -y build-essential cmake

mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"

sudo install -m 0755 mjpeg_streamer /usr/local/bin/mjpeg_streamer

if [ ! -f /etc/mjpeg-streamer.ini ]; then
    sudo cp ../config.example.ini /etc/mjpeg-streamer.ini
    echo ">>> edit /etc/mjpeg-streamer.ini and set server_host"
fi

sudo cp ../systemd/mjpeg-streamer.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable mjpeg-streamer.service
echo ">>> start:  sudo systemctl start mjpeg-streamer"
echo ">>> logs:   journalctl -u mjpeg-streamer -f"