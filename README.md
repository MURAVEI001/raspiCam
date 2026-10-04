# raspiCam

Захват MJPG с USB-камеры на Raspberry Pi и отправка кадров по UDP на MacBook (или любой другой хост).

## Зависимости

- Raspberry Pi OS (Bookworm или новее)
- CMake ≥ 3.16
- OpenCV ≥ 4.x с модулем `videoio` и поддержкой V4L2
- g++ с поддержкой C++17

Установка на Raspberry Pi OS:

```bash
sudo apt update
sudo apt install -y build-essential cmake libopencv-dev
```

## Сборка

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

## Запуск

```bash
./raspiCam <camera_id> <host_ip> <port> [device] [width] [height] [fps]
```

Примеры:

```bash
# Камера с ID=3 отправляет 1280x720@30 на 192.168.1.10:5000, /dev/video0
./raspiCam 3 192.168.1.10 5000

# Камера с ID=7, /dev/video1, 640x480, 15 FPS, порт 6000
./raspiCam 7 192.168.1.10 6000 1 640 480 15
```

`camera_id` — постоянный идентификатор конкретной камеры. Он попадает в заголовок каждого UDP-пакета, чтобы приёмник мог различать потоки от разных Raspberry Pi.

## Формат UDP-пакета

Все многобайтовые поля — в сетевом порядке байт (big-endian).

| Поле       | Тип      | Смещение | Описание                         |
|------------|----------|----------|----------------------------------|
| magic      | uint32   | 0        | Всегда `0x52415350` ("RASP")     |
| camera_id  | uint16   | 4        | ID камеры                        |
| frame_id   | uint32   | 6        | Порядковый номер кадра           |
| total_size | uint32   | 10       | Полный размер JPEG-кадра         |
| chunk_off  | uint32   | 14       | Смещение фрагмента в кадре       |
| chunk_len  | uint32   | 18       | Длина фрагмента в этом пакете    |
| payload    | bytes    | 22       | Фрагмент JPEG-данных             |

Один кадр JPEG обычно укладывается в один UDP-пакет при разрешении до 1280x720. Если кадр больше 1400 байт (типичный безопасный предел для Ethernet), он автоматически разбивается на несколько датаграмм.

## Как принимать на MacBook

Простой пример на Python (сохраняет кадры в `frame_<camera_id>.jpg`):

```python
import socket, struct, os

MAGIC = 0x52415350
HDR = struct.Struct(">IHIIII")  # magic, camera_id, frame_id, total, off, len
PAYLOAD_MAX = 1400

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", 5000))
print("Listening on UDP 5000...")

buffers = {}  # camera_id -> {frame_id, total, bytearray}

while True:
    data, addr = sock.recvfrom(65536)
    if len(data) < HDR.size:
        continue
    magic, cam_id, frame_id, total, off, clen = HDR.unpack_from(data, 0)
    if magic != MAGIC:
        continue
    payload = data[HDR.size:HDR.size + clen]

    key = (cam_id, frame_id)
    if key not in buffers or buffers[key][2] is None or len(buffers[key][2]) != total:
        buffers[key] = [total, bytearray(total), set()]

    total, buf, seen = buffers[key]
    buf[off:off + clen] = payload
    seen.add(off)

    # Проверяем, собрали ли все чанки (упрощённо: первый чанк содержит off=0)
    if off == 0:
        # Считаем, что кадр собран, если пришёл чанк с off == 0
        # и размер буфера совпадает с total.
        # В реальном проекте стоит проверять все ожидаемые смещения.
        with open(f"frame_{cam_id}.jpg", "wb") as f:
            f.write(buf[:total])
        print(f"[cam {cam_id}] frame {frame_id} saved ({total} bytes)")
        del buffers[key]
```

Это минимальный пример. В бою на соревнованиях имеет смысл:

1. Проверять, что собраны **все** чанки кадра (множество `seen` покрывает все смещения).
2. Хранить буферы в `collections.OrderedDict` с ограничением размера, чтобы не течь по памяти.
3. Писать кадры в отдельные потоки или через `asyncio`, чтобы приём не блокировался диском.

## Надёжность и проверки

- Проверяется, открылась ли камера.
- Проверяется, что камера реально поддерживает **MJPG** — если нет, программа завершается с внятной ошибкой.
- Проверяются все аргументы командной строки.
- Обрабатываются `SIGINT`/`SIGTERM` — программа завершается по Ctrl+C, не оставляя висящих сокетов.
- UDP-сокет использует RAII: закрывается в деструкторе даже при исключении.
- При ошибке `sendto()` программа не падает, а логирует проблему и продолжает работу — одна потерянная датаграмма не должна ронять соревнование.
- Цикл захвата стабилизирует FPS через `sleep_until`, чтобы не забивать сеть с максимальной скоростью.

## Ограничение текущей реализации

`cv::VideoCapture` с `CAP_V4L2` и `FOURCC=MJPG` **декодирует** MJPG в BGR-матрицу. OpenCV не отдаёт сырой JPEG-битстрим через `VideoCapture`. Поэтому кадр перекодируется обратно в JPEG через `cv::imencode` с качеством 85.

Для соревнований этого обычно достаточно: задержка минимальна, качество контролируемо. Если нужна максимальная производительность и минимальная латентность, можно перейти на прямой V4L2-захват с `V4L2_PIX_FMT_MJPEG` и `mmap` — тогда JPEG-битстрим забирается из драйвера без декодирования и повторного сжатия. Это сложнее в реализации, но даёт выигрыш по CPU и задержке.