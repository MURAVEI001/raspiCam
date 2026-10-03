#include "v4l2_capture.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstring>
#include <cerrno>
#include <iostream>

int V4L2Capture::xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

V4L2Capture::V4L2Capture(const std::string& device, int width, int height, int buffer_count) {
    fd_ = open(device.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd_ == -1)
        throw std::runtime_error("open(" + device + "): " + strerror(errno));

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = width;
    fmt.fmt.pix.height      = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) == -1)
        throw std::runtime_error("VIDIOC_S_FMT: " + std::string(strerror(errno)));

    v4l2_requestbuffers req{};
    req.count  = buffer_count;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) == -1)
        throw std::runtime_error("VIDIOC_REQBUFS: " + std::string(strerror(errno)));

    buffers_.resize(req.count);
    for (size_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) == -1)
            throw std::runtime_error("VIDIOC_QUERYBUF: " + std::string(strerror(errno)));

        buffers_[i].length = buf.length;
        buffers_[i].start  = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd_, buf.m.offset);
        if (buffers_[i].start == MAP_FAILED)
            throw std::runtime_error("mmap: " + std::string(strerror(errno)));
    }

    startStreaming();
}

V4L2Capture::~V4L2Capture() {
    try { stopStreaming(); } catch (...) {}
    for (auto& b : buffers_)
        if (b.start && b.start != MAP_FAILED) munmap(b.start, b.length);
    if (fd_ != -1) close(fd_);
}

void V4L2Capture::startStreaming() {
    for (size_t i = 0; i < buffers_.size(); ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd_, VIDIOC_QBUF, &buf) == -1)
            throw std::runtime_error("VIDIOC_QBUF: " + std::string(strerror(errno)));
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) == -1)
        throw std::runtime_error("VIDIOC_STREAMON: " + std::string(strerror(errno)));
    streaming_ = true;
}

void V4L2Capture::stopStreaming() {
    if (!streaming_) return;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);
    streaming_ = false;
}

bool V4L2Capture::waitFrame(const void** out_data, size_t* out_size, int timeout_ms) {
    // Простейший вариант: poll с таймаутом
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);
    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int r = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
    if (r <= 0) return false;

    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &buf) == -1) return false;

    current_index_ = buf.index;
    *out_data = buffers_[buf.index].start;
    *out_size = buf.bytesused;
    return true;
}

void V4L2Capture::releaseFrame() {
    if (current_index_ < 0) return;
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = current_index_;
    xioctl(fd_, VIDIOC_QBUF, &buf);
    current_index_ = -1;
}