/*
 * raspiCam — прямой V4L2 MJPEG-захват и отправка по UDP.
 *
 * Драйвер отдаёт уже сжатый JPEG, мы его не декодируем и не перекодируем.
 * На выходе — те же 22-байтовые заголовки и тот же протокол, что раньше.
 *
 * Запуск:
 *     ./raspiCam <camera_id> <host_ip> <port> [device] [width] [height] [fps] [quality]
 *
 * quality — 0..100, влияет на JPEG на стороне драйвера, по умолчанию 80.
 */

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kMagic         = 0x52415350;
constexpr int      kMaxUdpPayload = 1400;

std::atomic<bool> g_running{true};
void onSignal(int) { g_running = false; }

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic;
    uint16_t camera_id;
    uint32_t frame_id;
    uint32_t total_size;
    uint32_t chunk_off;
    uint32_t chunk_len;
};
#pragma pack(pop)
static_assert(sizeof(FrameHeader) == 22, "FrameHeader must be exactly 22 bytes");

// ---------------------------------------------------------------------------
class UdpSender {
public:
    UdpSender(const std::string& host, uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0)
            throw std::runtime_error(std::string("socket(): ") + std::strerror(errno));

        // Большой send buffer, чтобы sendto не блокировался при всплесках.
        int sndbuf = 4 * 1024 * 1024;
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        std::memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_port   = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr_.sin_addr) != 1) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("inet_pton failed for " + host);
        }
    }
    ~UdpSender() { if (fd_ >= 0) ::close(fd_); }
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    bool send(const void* data, size_t len) {
        ssize_t n = ::sendto(fd_, data, len, 0,
                             reinterpret_cast<const sockaddr*>(&addr_), sizeof(addr_));
        return n == static_cast<ssize_t>(len);
    }

private:
    int fd_{-1};
    sockaddr_in addr_{};
};

// ---------------------------------------------------------------------------
class V4L2MjpegCamera {
public:
    V4L2MjpegCamera(const std::string& dev, int w, int h, int fps, int quality) {
        fd_ = ::open(dev.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0)
            throw std::runtime_error("open " + dev + ": " + std::strerror(errno));

        v4l2_capability cap{};
        if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0)
            fail("VIDIOC_QUERYCAP");
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
            fail("device is not a video capture device");
        if (!(cap.capabilities & V4L2_CAP_STREAMING))
            fail("device does not support streaming I/O");

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = static_cast<uint32_t>(w);
        fmt.fmt.pix.height      = static_cast<uint32_t>(h);
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        fmt.fmt.pix.field       = V4L2_FIELD_ANY;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) fail("VIDIOC_S_FMT");

        if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
            char fourcc[5] = {0};
            std::memcpy(fourcc, &fmt.fmt.pix.pixelformat, 4);
            throw std::runtime_error(
                std::string("driver refused MJPEG, got: ") + fourcc);
        }

        // Явно попросим fps; если драйвер не умеет — не падаем, просто предупредим.
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(fps);
        if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
            std::cerr << "[cam] VIDIOC_S_PARM failed, using driver default fps\n";
        }

        // Некоторые UVC-камеры поддерживают "compression quality" (V4L2_CID_JPEG_COMPRESSION_QUALITY).
        // Попробуем выставить, ошибку игнорируем.
        v4l2_control ctrl{};
        ctrl.id    = V4L2_CID_JPEG_COMPRESSION_QUALITY;
        ctrl.value = quality;
        ioctl(fd_, VIDIOC_S_CTRL, &ctrl);

        // MMAP-буферы.
        v4l2_requestbuffers req{};
        req.count  = 4;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) fail("VIDIOC_REQBUFS");
        if (req.count < 2) fail("driver gave < 2 buffers");

        bufs_.resize(req.count);
        for (size_t i = 0; i < req.count; ++i) {
            v4l2_buffer b{};
            b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index  = static_cast<uint32_t>(i);
            if (ioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) fail("VIDIOC_QUERYBUF");

            bufs_[i].length = b.length;
            bufs_[i].start  = ::mmap(nullptr, b.length,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd_, b.m.offset);
            if (bufs_[i].start == MAP_FAILED) fail("mmap");

            if (ioctl(fd_, VIDIOC_QBUF, &b) < 0) fail("VIDIOC_QBUF");
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) fail("VIDIOC_STREAMON");

        std::cout << "[cam] " << dev << " " << w << "x" << h
                  << " MJPEG fps=" << fps << " quality=" << quality << "\n";
    }

    ~V4L2MjpegCamera() {
        if (fd_ >= 0) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_, VIDIOC_STREAMOFF, &type);
            for (auto& b : bufs_) {
                if (b.start && b.start != MAP_FAILED)
                    ::munmap(b.start, b.length);
            }
            ::close(fd_);
        }
    }

    V4L2MjpegCamera(const V4L2MjpegCamera&) = delete;
    V4L2MjpegCamera& operator=(const V4L2MjpegCamera&) = delete;

    // Блокируется до готовности кадра, максимум timeout_ms.
    // Возвращает указатель на JPEG внутри mmap-буфера и его размер.
    // После обработки обязательно вызвать release().
    const uint8_t* capture(size_t& size, int timeout_ms) {
        pollfd pfd{fd_, POLLIN, 0};
        int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr <= 0) return nullptr;

        v4l2_buffer b{};
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) return nullptr;
            std::cerr << "[cam] VIDIOC_DQBUF: " << std::strerror(errno) << "\n";
            return nullptr;
        }

        held_index_ = static_cast<int>(b.index);
        size        = b.bytesused;
        return static_cast<const uint8_t*>(bufs_[b.index].start);
    }

    void release() {
        if (held_index_ < 0) return;
        v4l2_buffer b{};
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = static_cast<uint32_t>(held_index_);
        if (ioctl(fd_, VIDIOC_QBUF, &b) < 0)
            std::cerr << "[cam] VIDIOC_QBUF: " << std::strerror(errno) << "\n";
        held_index_ = -1;
    }

private:
    struct Buf { void* start{nullptr}; size_t length{0}; };

    [[noreturn]] void fail(const char* what) {
        const std::string err = std::strerror(errno);
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        throw std::runtime_error(std::string(what) + ": " + err);
    }

    int              fd_{-1};
    std::vector<Buf> bufs_;
    int              held_index_{-1};
};

// ---------------------------------------------------------------------------
void sendFrame(UdpSender& sender, uint16_t camera_id,
               uint32_t frame_id, const uint8_t* jpeg, size_t total) {
    std::vector<uint8_t> packet(sizeof(FrameHeader) + kMaxUdpPayload);

    size_t offset = 0;
    while (offset < total) {
        const size_t chunk = std::min<size_t>(kMaxUdpPayload, total - offset);

        FrameHeader hdr{};
        hdr.magic      = htonl(kMagic);
        hdr.camera_id  = htons(camera_id);
        hdr.frame_id   = htonl(frame_id);
        hdr.total_size = htonl(static_cast<uint32_t>(total));
        hdr.chunk_off  = htonl(static_cast<uint32_t>(offset));
        hdr.chunk_len  = htonl(static_cast<uint32_t>(chunk));

        std::memcpy(packet.data(), &hdr, sizeof(hdr));
        std::memcpy(packet.data() + sizeof(hdr), jpeg + offset, chunk);

        if (!sender.send(packet.data(), sizeof(hdr) + chunk)) {
            std::cerr << "[cam " << camera_id << "] send failed at off=" << offset << "\n";
            return;
        }
        offset += chunk;
    }
}

} // namespace

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr
            << "Usage: " << argv[0]
            << " <camera_id> <host_ip> <port> [device] [width] [height] [fps] [quality]\n"
            << "  device : /dev/videoN (по умолчанию 0)\n"
            << "  width  : 1280\n"
            << "  height : 720\n"
            << "  fps    : 30\n"
            << "  quality: 80 (JPEG, 0..100)\n";
        return 1;
    }

    auto parse_int = [](const char* s) -> int {
        try { return std::stoi(s); }
        catch (...) { throw std::runtime_error(std::string("bad int: ") + s); }
    };

    int camera_id = 0, port = 0, device = 0;
    int width = 1280, height = 720, fps = 30, quality = 80;
    std::string host;

    try {
        camera_id = parse_int(argv[1]);
        host      = argv[2];
        port      = parse_int(argv[3]);
        if (argc > 4) device  = parse_int(argv[4]);
        if (argc > 5) width   = parse_int(argv[5]);
        if (argc > 6) height  = parse_int(argv[6]);
        if (argc > 7) fps     = parse_int(argv[7]);
        if (argc > 8) quality = parse_int(argv[8]);
    } catch (const std::exception& e) {
        std::cerr << "argument error: " << e.what() << "\n";
        return 1;
    }

    if (camera_id < 0 || camera_id > 65535) { std::cerr << "camera_id 0..65535\n"; return 1; }
    if (port      <= 0 || port      > 65535) { std::cerr << "port 1..65535\n"; return 1; }
    if (device    <  0)                      { std::cerr << "device >= 0\n"; return 1; }
    if (width <= 0 || height <= 0 || fps <= 0) { std::cerr << "w/h/fps > 0\n"; return 1; }
    if (quality < 1 || quality > 100)         { std::cerr << "quality 1..100\n"; return 1; }

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        UdpSender sender(host, static_cast<uint16_t>(port));
        std::cout << "[net] UDP → " << host << ":" << port << "\n";

        V4L2MjpegCamera cam("/dev/video" + std::to_string(device),
                            width, height, fps, quality);

        uint32_t frame_id = 0;
        size_t   last_size = 0;
        auto     last_stat = std::chrono::steady_clock::now();
        uint64_t frames_this_sec = 0;
        uint64_t bytes_this_sec  = 0;

        while (g_running) {
            size_t size = 0;
            const uint8_t* jpeg = cam.capture(size, 1000);
            if (!jpeg) {
                std::cerr << "[cam] capture timeout/error\n";
                continue;
            }

            sendFrame(sender, static_cast<uint16_t>(camera_id), frame_id++, jpeg, size);
            cam.release();

            last_size = size;
            ++frames_this_sec;
            bytes_this_sec += size;

            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stat).count() >= 1) {
                std::cout << "[cam " << camera_id << "] "
                          << frames_this_sec << " fps, "
                          << bytes_this_sec / 1024 << " KiB/s, last="
                          << last_size << " B\n";
                frames_this_sec = 0;
                bytes_this_sec  = 0;
                last_stat       = now;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << "\n";
        return 1;
    }
    return 0;
}