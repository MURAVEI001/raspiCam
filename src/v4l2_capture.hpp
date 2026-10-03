#pragma once
#include <string>
#include <vector>
#include <cstddef>
#include <linux/videodev2.h>

// Обёртка над V4L2 в режиме MJPEG.
// Отдаёт уже готовые JPEG-байты — без декодирования.
class V4L2Capture {
public:
    V4L2Capture(const std::string& device, int width, int height, int buffer_count = 4);
    ~V4L2Capture();

    // Не копирует: возвращает указатель на mmap-буфер и его размер.
    // Валиден до следующего вызова releaseFrame().
    bool waitFrame(const void** out_data, size_t* out_size, int timeout_ms = 1000);

    // Возвращает буфер в очередь драйвера — обязательно вызывать после обработки.
    void releaseFrame();

private:
    struct Buffer { void* start; size_t length; };

    int fd_ = -1;
    std::vector<Buffer> buffers_;
    int current_index_ = -1;
    bool streaming_ = false;

    void startStreaming();
    void stopStreaming();
    static int xioctl(int fd, unsigned long req, void* arg);
};