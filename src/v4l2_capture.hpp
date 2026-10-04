#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

namespace mjpeg {

// Обёртка над V4L2 с mmap-буферами. Отдаёт указатель на последний готовый MJPEG-кадр.
class V4L2Capture {
public:
    V4L2Capture() = default;
    ~V4L2Capture();

    V4L2Capture(const V4L2Capture&) = delete;
    V4L2Capture& operator=(const V4L2Capture&) = delete;

    // Открывает устройство и настраивает формат. При ошибке возвращает false и пишет в лог.
    bool open_device(const std::string& dev,
                     int width, int height, int fps, int buffer_count);

    // Захватывает один кадр. Блокируется до появления кадра или таймаута.
    // Возвращает указатель на данные кадра и его размер, либо {nullptr, 0} при ошибке/таймауте.
    struct Frame {
        const uint8_t* data = nullptr;
        size_t         size = 0;
    };
    Frame capture(int timeout_ms);

    // Возвращает буфер обратно в очередь драйвера. ОБЯЗАТЕЛЬНО вызывать после обработки.
    void release();

    void close_device();

    bool is_open() const { return fd_ >= 0; }

    // Открыт ли физически поток (есть ли кадры)
    bool streaming() const { return streaming_; }

private:
    int fd_ = -1;
    bool streaming_ = false;
    struct Buffer {
        void*  start = nullptr;
        size_t length = 0;
    };
    std::vector<Buffer> buffers_;
    int last_index_ = -1;
};

} // namespace mjpeg