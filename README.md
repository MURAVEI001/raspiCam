# raspiCam

Программа для Raspberry Pi 3, захватывающая MJPEG с камеры OV2710 (или любой UVC-камеры, поддерживающей MJPEG) и отправляющая кадры на MacBook по TCP.

## Сборка

```bash
sudo apt update
sudo apt install -y build-essential cmake

mkdir build && cd build
cmake ..
make -j4

# плата с 1 камерой
./raspiCam 192.168.2.1 9000 front=/dev/video0

# плата с 2 камерами
./raspiCam 192.168.2.1 9000 turret=/dev/video0 turret_wide=/dev/video2