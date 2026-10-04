#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <linux/videodev2.h>

class CameraCapture {
public:
    CameraCapture();
    ~CameraCapture();

    // Открыть устройство и настроить формат MJPEG (V4L2_PIX_FMT_JPEG / MJPEG)
    bool open(const std::string& device, uint32_t width, uint32_t height, uint32_t fps);
    void close();

    // Захватить один кадр. Возвращает указатель на данные и размер.
    // Данные действительны до следующего вызова capture() или close().
    bool capture(const uint8_t** data, size_t* size);

    bool isOpen() const { return fd_ >= 0; }

private:
    void dumpSupportedFormats(const std::string& device);
    struct Buffer {
        void*  start;
        size_t length;
    };

    int fd_ = -1;
    std::vector<Buffer> buffers_;
    bool streaming_ = false;

    bool initMmap();
    bool startStreaming();
    void stopStreaming();
    void uninitMmap();
};