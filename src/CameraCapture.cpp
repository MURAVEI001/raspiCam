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

    // Пробуем MJPEG (UVC), при неудаче — JPEG (некоторые драйверы)
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = width;
    fmt.fmt.pix.height      = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;  // <<< ВОТ ГЛАВНОЕ ИСПРАВЛЕНИЕ
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        perror("CameraCapture::open: VIDIOC_S_FMT (MJPEG)");

        // Пробуем альтернативный fourcc
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            perror("CameraCapture::open: VIDIOC_S_FMT (JPEG)");
            dumpSupportedFormats(device);   // ← сам расскажет, что умеет камера
            close();
            return false;
        }
    }

    // Драйвер мог «подправить» формат — проверяем, что он реально вернул
    uint32_t actual_fourcc = fmt.fmt.pix.pixelformat;
    if (actual_fourcc != V4L2_PIX_FMT_MJPEG && actual_fourcc != V4L2_PIX_FMT_JPEG) {
        fprintf(stderr,
                "CameraCapture: драйвер вернул неожиданный формат '%c%c%c%c' "
                "(размер %ux%u)\n",
                (actual_fourcc      ) & 0xFF,
                (actual_fourcc >>  8) & 0xFF,
                (actual_fourcc >> 16) & 0xFF,
                (actual_fourcc >> 24) & 0xFF,
                fmt.fmt.pix.width, fmt.fmt.pix.height);
        dumpSupportedFormats(device);
        close();
        return false;
    }

    // Логируем фактически установленные параметры
    fprintf(stderr,
            "CameraCapture: формат '%c%c%c%c', %ux%u, bytesperline=%u, sizeimage=%u\n",
            (actual_fourcc      ) & 0xFF,
            (actual_fourcc >>  8) & 0xFF,
            (actual_fourcc >> 16) & 0xFF,
            (actual_fourcc >> 24) & 0xFF,
            fmt.fmt.pix.width,
            fmt.fmt.pix.height,
            fmt.fmt.pix.bytesperline,
            fmt.fmt.pix.sizeimage);

    // Установка частоты кадров (если поддерживается)
    struct v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
        // Не критично — многие UVC-камеры не дают менять FPS через этот интерфейс.
        fprintf(stderr, "CameraCapture: VIDIOC_S_PARM не поддержан, используется FPS по умолчанию\n");
    }

    // Запрос буферов
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
        fprintf(stderr, "CameraCapture: недостаточно буферов (%u)\n", req.count);
        close();
        return false;
    }

    if (!initMmap()) { close(); return false; }
    if (!startStreaming()) { close(); return false; }

    return true;
}

void CameraCapture::dumpSupportedFormats(const std::string& device) {
    fprintf(stderr, "\n=== Поддерживаемые форматы на %s ===\n", device.c_str());

    struct v4l2_fmtdesc fmtdesc = {};
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (fmtdesc.index = 0; ; ++fmtdesc.index) {
        if (ioctl(fd_, VIDIOC_ENUM_FMT, &fmtdesc) < 0) break;

        uint32_t f = fmtdesc.pixelformat;
        fprintf(stderr, "[%u] '%c%c%c%c'  %s\n",
                fmtdesc.index,
                (f      ) & 0xFF,
                (f >>  8) & 0xFF,
                (f >> 16) & 0xFF,
                (f >> 24) & 0xFF,
                fmtdesc.description);

        // Перечисляем размеры для этого формата
        struct v4l2_frmsizeenum frmsize = {};
        frmsize.pixel_format = f;
        for (frmsize.index = 0; ; ++frmsize.index) {
            if (ioctl(fd_, VIDIOC_ENUM_FRAMESIZES, &frmsize) < 0) break;

            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                fprintf(stderr, "     %ux%u\n",
                        frmsize.discrete.width, frmsize.discrete.height);

                // И FPS для каждого размера
                struct v4l2_frmivalenum frmival = {};
                frmival.pixel_format = f;
                frmival.width  = frmsize.discrete.width;
                frmival.height = frmsize.discrete.height;
                for (frmival.index = 0; ; ++frmival.index) {
                    if (ioctl(fd_, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) < 0) break;
                    if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                        if (frmival.discrete.numerator != 0) {
                            double fps_val = (double)frmival.discrete.denominator
                                           / (double)frmival.discrete.numerator;
                            fprintf(stderr, "        %.2f fps\n", fps_val);
                        }
                    }
                }
            } else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
                fprintf(stderr, "     stepwise: %ux%u .. %ux%u\n",
                        frmsize.stepwise.min_width,  frmsize.stepwise.min_height,
                        frmsize.stepwise.max_width,  frmsize.stepwise.max_height);
            }
        }
    }
    fprintf(stderr, "=====================================\n\n");
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