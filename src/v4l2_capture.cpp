#include "v4l2_capture.hpp"
#include "log.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <cstring>
#include <cerrno>
#include <cstdlib>

namespace mjpeg {

static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

V4L2Capture::~V4L2Capture() {
    close_device();
}

bool V4L2Capture::open_device(const std::string& dev,
                              int width, int height, int fps, int buffer_count) {
    close_device();

    struct stat st{};
    if (stat(dev.c_str(), &st) == -1) {
        LOG_ERR("stat(%s): %s", dev.c_str(), strerror(errno));
        return false;
    }
    if (!S_ISCHR(st.st_mode)) {
        LOG_ERR("%s is not a character device", dev.c_str());
        return false;
    }

    fd_ = ::open(dev.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0) {
        LOG_ERR("open(%s): %s", dev.c_str(), strerror(errno));
        return false;
    }

    // 1) Проверяем capabilities
    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERR("VIDIOC_QUERYCAP: %s", strerror(errno));
        close_device();
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        LOG_ERR("%s: no V4L2_CAP_VIDEO_CAPTURE", dev.c_str());
        close_device();
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        LOG_ERR("%s: no V4L2_CAP_STREAMING", dev.c_str());
        close_device();
        return false;
    }
    LOG_INFO("device: %s | driver: %s | card: %s",
             dev.c_str(), cap.driver, cap.card);

    // 2) Устанавливаем формат MJPEG
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = static_cast<uint32_t>(width);
    fmt.fmt.pix.height      = static_cast<uint32_t>(height);
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERR("VIDIOC_S_FMT MJPEG: %s", strerror(errno));
        close_device();
        return false;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        LOG_ERR("driver refused MJPEG, got fourcc=0x%08X",
                fmt.fmt.pix.pixelformat);
        close_device();
        return false;
    }
    if (fmt.fmt.pix.width != static_cast<uint32_t>(width) ||
        fmt.fmt.pix.height != static_cast<uint32_t>(height)) {
        LOG_WARN("driver adjusted size to %ux%u",
                 fmt.fmt.pix.width, fmt.fmt.pix.height);
    }
    LOG_INFO("format: MJPEG %ux%u", fmt.fmt.pix.width, fmt.fmt.pix.height);

    // 3) Устанавливаем FPS (не критично, но полезно)
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(fps);
    if (xioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
        LOG_WARN("VIDIOC_S_PARM: %s (continuing)", strerror(errno));
    }

    // 4) Запрашиваем mmap-буферы
    v4l2_requestbuffers req{};
    req.count  = static_cast<uint32_t>(buffer_count);
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERR("VIDIOC_REQBUFS: %s", strerror(errno));
        close_device();
        return false;
    }
    if (req.count < 2) {
        LOG_ERR("insufficient buffer memory (got %u)", req.count);
        close_device();
        return false;
    }

    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERR("VIDIOC_QUERYBUF[%u]: %s", i, strerror(errno));
            close_device();
            return false;
        }
        buffers_[i].length = buf.length;
        buffers_[i].start  = mmap(nullptr, buf.length,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd_, buf.m.offset);
        if (buffers_[i].start == MAP_FAILED) {
            buffers_[i].start = nullptr;
            LOG_ERR("mmap[%u]: %s", i, strerror(errno));
            close_device();
            return false;
        }
    }
    LOG_INFO("mapped %zu buffers", buffers_.size());

    // 5) Очередим буферы и включаем стриминг
    for (uint32_t i = 0; i < buffers_.size(); ++i) {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            LOG_ERR("VIDIOC_QBUF[%u]: %s", i, strerror(errno));
            close_device();
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERR("VIDIOC_STREAMON: %s", strerror(errno));
        close_device();
        return false;
    }
    streaming_ = true;
    LOG_INFO("streaming started");
    return true;
}

V4L2Capture::Frame V4L2Capture::capture(int timeout_ms) {
    if (!is_open() || !streaming_) return {};

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);
    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int r = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
    if (r < 0) {
        if (errno == EINTR) return {};
        LOG_ERR("select: %s", strerror(errno));
        return {};
    }
    if (r == 0) return {}; // таймаут

    v4l2_buffer buf{};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return {};
        LOG_ERR("VIDIOC_DQBUF: %s", strerror(errno));
        return {};
    }
    if (buf.index >= buffers_.size()) {
        LOG_ERR("DQBUF returned bad index %u", buf.index);
        return {};
    }
    if (!(buf.flags & V4L2_BUF_FLAG_DONE) && buf.bytesused == 0) {
        // странный буфер, вернём обратно
        xioctl(fd_, VIDIOC_QBUF, &buf);
        return {};
    }

    last_index_ = static_cast<int>(buf.index);
    return Frame{
        static_cast<const uint8_t*>(buffers_[buf.index].start),
        static_cast<size_t>(buf.bytesused)
    };
}

void V4L2Capture::release() {
    if (last_index_ < 0 || fd_ < 0) return;
    v4l2_buffer buf{};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = static_cast<uint32_t>(last_index_);
    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        LOG_WARN("VIDIOC_QBUF(release): %s", strerror(errno));
    }
    last_index_ = -1;
}

void V4L2Capture::close_device() {
    if (fd_ >= 0) {
        if (streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            xioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        for (auto& b : buffers_) {
            if (b.start && b.start != MAP_FAILED) {
                munmap(b.start, b.length);
            }
            b.start = nullptr;
        }
        buffers_.clear();
        ::close(fd_);
        fd_ = -1;
    }
    last_index_ = -1;
    streaming_ = false;
}

} // namespace mjpeg