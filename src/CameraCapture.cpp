#include "CameraCapture.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstring>
#include <cerrno>
#include <cstdio>

CameraCapture::CameraCapture() = default;

CameraCapture::~CameraCapture() {
    close();
}

bool CameraCapture::open(const std::string& device, uint32_t width, uint32_t height, uint32_t fps) {
    close();

    fd_ = ::open(device.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0) {
        perror("CameraCapture::open: open");
        return false;
    }

    // Проверка возможностей устройства
    struct v4l2_capability cap;
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("CameraCapture::open: VIDIOC_QUERYCAP");
        close();
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "CameraCapture: устройство не поддерживает видеозахват\n");
        close();
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "CameraCapture: устройство не поддерживает streaming I/O\n");
        close();
        return false;
    }

    // Установка формата MJPEG
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = width;
    fmt.fmt.pix.height      = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG; // OV2710 отдаёт MJPEG как JPEG
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        perror("CameraCapture::open: VIDIOC_S_FMT");
        close();
        return false;
    }

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_JPEG) {
        fprintf(stderr, "CameraCapture: драйвер не принял формат JPEG/MJPEG\n");
        close();
        return false;
    }

    // Установка частоты кадров (если поддерживается)
    struct v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    ioctl(fd_, VIDIOC_S_PARM, &parm); // не критично, если не поддерживается

    // Запрос буферов (4 буфера достаточно для сглаживания)
    struct v4l2_requestbuffers req = {};
    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        perror("CameraCapture::open: VIDIOC_REQBUFS");
        close();
        return false;
    }
    if (req.count < 2) {
        fprintf(stderr, "CameraCapture: недостаточно буферов\n");
        close();
        return false;
    }

    if (!initMmap()) {
        close();
        return false;
    }

    if (!startStreaming()) {
        close();
        return false;
    }

    return true;
}

void CameraCapture::close() {
    stopStreaming();
    uninitMmap();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool CameraCapture::initMmap() {
    struct v4l2_requestbuffers req = {};
    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    // Повторный запрос для получения фактического количества
    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        perror("CameraCapture::initMmap: VIDIOC_REQBUFS");
        return false;
    }

    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf = {};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("CameraCapture::initMmap: VIDIOC_QUERYBUF");
            return false;
        }

        buffers_[i].length = buf.length;
        buffers_[i].start  = mmap(nullptr, buf.length,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd_, buf.m.offset);

        if (buffers_[i].start == MAP_FAILED) {
            perror("CameraCapture::initMmap: mmap");
            return false;
        }
    }
    return true;
}

bool CameraCapture::startStreaming() {
    for (uint32_t i = 0; i < buffers_.size(); ++i) {
        struct v4l2_buffer buf = {};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            perror("CameraCapture::startStreaming: VIDIOC_QBUF");
            return false;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        perror("CameraCapture::startStreaming: VIDIOC_STREAMON");
        return false;
    }
    streaming_ = true;
    return true;
}

void CameraCapture::stopStreaming() {
    if (!streaming_) return;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_, VIDIOC_STREAMOFF, &type);
    streaming_ = false;
}

void CameraCapture::uninitMmap() {
    for (auto& b : buffers_) {
        if (b.start && b.start != MAP_FAILED) {
            munmap(b.start, b.length);
        }
    }
    buffers_.clear();
}

bool CameraCapture::capture(const uint8_t** data, size_t* size) {
    if (fd_ < 0 || !streaming_) return false;

    struct v4l2_buffer buf = {};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    // Ожидание кадра (блокирующий вызов с таймаутом через poll)
    // Для простоты используем блокирующий DQBUF; в реальном коде лучше poll.
    if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return false; // нет кадра
        perror("CameraCapture::capture: VIDIOC_DQBUF");
        return false;
    }

    *data = static_cast<const uint8_t*>(buffers_[buf.index].start);
    *size = buf.bytesused;

    // Немедленно возвращаем буфер в очередь
    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        perror("CameraCapture::capture: VIDIOC_QBUF");
        return false;
    }

    return true;
}