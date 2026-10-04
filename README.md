# raspiCam

Программа для Raspberry Pi 3, захватывающая MJPEG с камеры OV2710 (или любой UVC-камеры, поддерживающей MJPEG) и отправляющая кадры на MacBook по TCP.

## Сборка

```bash
sudo apt update
sudo apt install -y build-essential cmake

mkdir build && cd build
cmake ..
make -j4