# rpi-mjpeg-streamer

Минималистичный, производительный стример MJPEG-кадров с Raspberry Pi 3
(Raspi OS Lite) на MacBook по Ethernet. Без перекодирования: камера уже
отдаёт MJPEG, мы лишь читаем её через V4L2 и отправляем по TCP.

## Протокол

Каждый кадр — пакет:

```
[uint32 BE length][JPEG data]
```

Длина — размер JPEG в байтах. Сервер на Mac читает 4 байта, потом `length`
байт, и так далее.

## Сборка на Raspberry Pi

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake
git clone <this-repo> rpi-mjpeg-streamer
cd rpi-mjpeg-streamer
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

## Настройка

```bash
sudo cp ../config.example.ini /etc/mjpeg-streamer.ini
sudo nano /etc/mjpeg-streamer.ini   # укажите server_host = IP MacBook
```

## Быстрый запуск вручную

```bash
./build/mjpeg_streamer --config /etc/mjpeg-streamer.ini
```

Вы должны увидеть что-то вроде:

```
[12:00:01][INFO] device: /dev/video0 | driver: uvcvideo | card: OV2710
[12:00:01][INFO] format: MJPEG 1920x1080
[12:00:01][INFO] mapped 4 buffers
[12:00:01][INFO] streaming started
[12:00:01][INFO] connected to 192.168.1.10:9000
[12:00:02][INFO] fps=30.0  mbps=24.10  frames=30
```

## Установка как systemd-сервис

```bash
./scripts/install.sh
sudo systemctl start mjpeg-streamer
journalctl -u mjpeg-streamer -f
```

## Настройки производительности (важно для 100 Мбит Ethernet)

- **1920x1080 MJPEG @ 30 fps** ≈ 20–40 Мбит/с. Это близко к пределу
  100 Мбит Ethernet, но всё ещё проходит. Если видите просадки — снизьте
  `fps` до 20 или уменьшите `width`/`height`.
- `TCP_NODELAY` включён, чтобы не было задержек из-за алгоритма Нейгла.
- `SO_SNDBUF` увеличен до 1 МиБ.
- JPEG-кадры **не копируются** — читаем прямо из mmap-буфера ядра,
  пишем в сокет, возвращаем буфер.

## Что делать, если что-то падает

1. Проверьте `journalctl -u mjpeg-streamer -f` — там все ошибки с `errno`.
2. Проверьте камеру: `v4l2-ctl --list-formats-ext -d /dev/video0`.
   Убедитесь, что MJPEG 1920x1080 поддерживается.
3. Проверьте сеть: `ping <IP_MacBook>`, `ip a`.
4. Убедитесь, что на Mac слушает сервер на нужном порту.

## Тестовый приёмник на Mac (для проверки)

Быстрый способ убедиться, что данные идут — небольшой Python-скрипт:

```python
# receiver.py
import socket, struct, sys, os

HOST, PORT = "0.0.0.0", 9000
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((HOST, PORT))
s.listen(4)
print(f"listening on {HOST}:{PORT}")

conn, addr = s.accept()
print("connected:", addr)
os.makedirs("frames", exist_ok=True)

n = 0
try:
    while True:
        hdr = b""
        while len(hdr) < 4:
            chunk = conn.recv(4 - len(hdr))
            if not chunk:
                sys.exit(0)
            hdr += chunk
        (size,) = struct.unpack(">I", hdr)

        data = b""
        while len(data) < size:
            chunk = conn.recv(min(65536, size - len(data)))
            if not chunk:
                sys.exit(0)
            data += chunk

        n += 1
        if n % 30 == 0:
            print(f"frame {n}: {size} bytes")
        with open("frames/last.jpg", "wb") as f:
            f.write(data)
except KeyboardInterrupt:
    pass
```

Откройте `frames/last.jpg` — это должен быть живой кадр.

## Лицензия

MIT