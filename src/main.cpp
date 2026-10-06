/*
 * raspiCam — прямой V4L2 MJPEG-захват и отправка по UDP.
 *
 * Драйвер отдаёт уже сжатый JPEG, мы его не декодируем и не перекодируем.
 * На выходе — те же 22-байтовые заголовки и тот же протокол, что раньше.
 *
 * Запуск:
 *     ./raspiCam <camera_id> <host_ip> <port>
 *                [device] [width] [height] [fps] [quality] [exposure] [gain]
 *
 * quality  — 0..100, влияет на JPEG на стороне драйвера, по умолчанию 80.
 * exposure — значение V4L2_CID_EXPOSURE_ABSOLUTE, -1 = не трогать (авто).
 * gain     — значение V4L2_CID_GAIN,               -1 = не трогать (авто).
 *
 * Порядок аргументов после port:
 *     device width height fps quality exposure gain
 *     /dev/videoN — по умолчанию 0
 *     width=1280 height=720 fps=30 quality=80 exposure=-1 gain=-1
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
    V4L2MjpegCamera(const std::string& dev, int w, int h, int fps, int quality,
                    int exposure = -1, int gain = -1)
    {
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

        // --- JPEG quality (некоторые UVC-камеры поддерживают) ---
        if (quality > 0) {
            v4l2_control ctrl{};
            ctrl.id    = V4L2_CID_JPEG_COMPRESSION_QUALITY;
            ctrl.value = quality;
            if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
                std::cerr << "[cam] JPEG quality not supported (ignored)\n";
            } else {
                std::cout << "[cam] jpeg quality = " << quality << "\n";
            }
        }

        // --- Exposure ---
        // exposure < 0  -> оставляем авторежим.
        // exposure >= 0 -> ручной режим с указанным значением.
        if (exposure >= 0) {
            // Шаг 1: отключаем авто-выдержку.
            v4l2_control auto_ctrl{};
            auto_ctrl.id    = V4L2_CID_EXPOSURE_AUTO;
            auto_ctrl.value = V4L2_EXPOSURE_MANUAL;
            if (ioctl(fd_, VIDIOC_S_CTRL, &auto_ctrl) < 0) {
                std::cerr << "[cam] VIDIOC_S_CTRL(EXPOSURE_AUTO=MANUAL) failed: "
                          << std::strerror(errno)
                          << " — камера может не поддерживать ручную выдержку\n";
            }

            // Шаг 2: узнаём допустимый диапазон (у разных камер он разный).
            v4l2_queryctrl q{};
            q.id = V4L2_CID_EXPOSURE_ABSOLUTE;
            if (ioctl(fd_, VIDIOC_QUERYCTRL, &q) == 0 &&
                !(q.flags & V4L2_CTRL_FLAG_DISABLED))
            {
                int val = exposure;
                if (val < q.minimum) val = q.minimum;
                if (val > q.maximum) val = q.maximum;
                if (q.step > 1)
                    val = q.minimum + ((val - q.minimum) / q.step) * q.step;

                v4l2_control exp_ctrl{};
                exp_ctrl.id    = V4L2_CID_EXPOSURE_ABSOLUTE;
                exp_ctrl.value = val;
                if (ioctl(fd_, VIDIOC_S_CTRL, &exp_ctrl) < 0) {
                    std::cerr << "[cam] VIDIOC_S_CTRL(EXPOSURE_ABSOLUTE=" << val
                              << ") failed: " << std::strerror(errno) << "\n";
                } else {
                    std::cout << "[cam] exposure = " << val
                              << " (range " << q.minimum << ".." << q.maximum
                              << ", step "   << q.step
                              << ", default " << q.default_value << ")\n";
                }
            } else {
                std::cerr << "[cam] EXPOSURE_ABSOLUTE not supported by this camera\n";
            }
        }

        // --- Gain ---
        // gain < 0  -> авто.
        // gain >= 0 -> ручной.
        //
        // ВАЖНО: у UVC-камер обычно есть ДВА контрола:
        //   V4L2_CID_AUTOGAIN  — вкл/выкл автоматики (0/1)
        //   V4L2_CID_GAIN      — само значение усиления
        // Отключаем авто, потом ставим значение.
        if (gain >= 0) {
            v4l2_control autogain{};
            autogain.id    = V4L2_CID_AUTOGAIN;
            autogain.value = 0;   // 0 = manual
            if (ioctl(fd_, VIDIOC_S_CTRL, &autogain) < 0) {
                std::cerr << "[cam] VIDIOC_S_CTRL(AUTOGAIN=0) failed: "
                          << std::strerror(errno) << "\n";
            }

            v4l2_queryctrl qg{};
            qg.id = V4L2_CID_GAIN;
            if (ioctl(fd_, VIDIOC_QUERYCTRL, &qg) == 0 &&
                !(qg.flags & V4L2_CTRL_FLAG_DISABLED))
            {
                int val = gain;
                if (val < qg.minimum) val = qg.minimum;
                if (val > qg.maximum) val = qg.maximum;
                if (qg.step > 1)
                    val = qg.minimum + ((val - qg.minimum) / qg.step) * qg.step;

                v4l2_control gctrl{};
                gctrl.id    = V4L2_CID_GAIN;
                gctrl.value = val;
                if (ioctl(fd_, VIDIOC_S_CTRL, &gctrl) < 0) {
                    std::cerr << "[cam] VIDIOC_S_CTRL(GAIN=" << val
                              << ") failed: " << std::strerror(errno) << "\n";
                } else {
                    std::cout << "[cam] gain = " << val
                              << " (range " << qg.minimum << ".." << qg.maximum
                              << ", step "  << qg.step
                              << ", default " << qg.default_value << ")\n";
                }
            } else {
                std::cerr << "[cam] GAIN not supported by this camera\n";
            }
        }

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
                  << " MJPEG fps=" << fps
                  << " quality=" << quality
                  << " exposure=" << (exposure >= 0 ? std::to_string(exposure) : "auto")
                  << " gain="     << (gain     >= 0 ? std::to_string(gain)     : "auto")
                  << "\n";
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
            << " <camera_id> <host_ip> <port>\n"
            << "       [device] [width] [height] [fps] [quality] [exposure] [gain]\n"
            << "  device  : /dev/videoN (по умолчанию 0)\n"
            << "  width   : 1280\n"
            << "  height  : 720\n"
            << "  fps     : 30\n"
            << "  quality : 80 (JPEG, 0..100)\n"
            << "  exposure: значение V4L2_CID_EXPOSURE_ABSOLUTE, -1 = авто (по умолчанию)\n"
            << "  gain    : значение V4L2_CID_GAIN,               -1 = авто (по умолчанию)\n";
        return 1;
    }

    auto parse_int = [](const char* s) -> int {
        try { return std::stoi(s); }
        catch (...) { throw std::runtime_error(std::string("bad int: ") + s); }
    };

    int camera_id = 0, port = 0, device = 0;
    int width = 1280, height = 720, fps = 30, quality = 80;
    int exposure = -1;
    int gain     = -1;
    std::string host;

    try {
        camera_id = parse_int(argv[1]);
        host      = argv[2];
        port      = parse_int(argv[3]);
        if (argc > 4)  device   = parse_int(argv[4]);
        if (argc > 5)  width    = parse_int(argv[5]);
        if (argc > 6)  height   = parse_int(argv[6]);
        if (argc > 7)  fps      = parse_int(argv[7]);
        if (argc > 8)  quality  = parse_int(argv[8]);
        if (argc > 9)  exposure = parse_int(argv[9]);
        if (argc > 10) gain     = parse_int(argv[10]);
    } catch (const std::exception& e) {
        std::cerr << "argument error: " << e.what() << "\n";
        return 1;
    }

    if (camera_id < 0 || camera_id > 65535) { std::cerr << "camera_id 0..65535\n"; return 1; }
    if (port      <= 0 || port      > 65535) { std::cerr << "port 1..65535\n"; return 1; }
    if (device    <  0)                      { std::cerr << "device >= 0\n"; return 1; }
    if (width <= 0 || height <= 0 || fps <= 0) { std::cerr << "w/h/fps > 0\n"; return 1; }
    if (quality < 0 || quality > 100)         { std::cerr << "quality 0..100\n"; return 1; }
    if (exposure < -1)                        { std::cerr << "exposure >= -1\n"; return 1; }
    if (gain     < -1)                        { std::cerr << "gain >= -1\n"; return 1; }

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        UdpSender sender(host, static_cast<uint16_t>(port));
        std::cout << "[net] UDP → " << host << ":" << port << "\n";

        V4L2MjpegCamera cam("/dev/video" + std::to_string(device),
                            width, height, fps, quality, exposure, gain);

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