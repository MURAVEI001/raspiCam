// raspiCam.cpp
//
// High-performance MJPEG capture from V4L2 cameras on Raspberry Pi,
// streamed over UDP to a receiver (e.g. MacBook over Ethernet).
//
// Build (release, native):
//   g++ -O3 -march=native -std=c++17 -pthread -o raspiCam raspiCam.cpp
//
// Usage:
//   ./raspiCam --host <ip> --port <port> \
//              --camera <id>:<device>[:<w>:<h>:<fps>] [--camera ...]
//
// Example:
//   ./raspiCam --host 192.168.1.10 --port 5000 \
//              --camera 1:/dev/video0:1920:1080:30
//
// UDP packet format (all integers little-endian, packed, 16-byte header):
//   magic         uint32  = 0x4D414352 ("RCAM")
//   camera_id     uint16
//   total_packets uint16  (total packets for current frame_id)
//   frame_id      uint32  (monotonic per camera, wraps)
//   packet_index  uint16  (0..total_packets-1)
//   payload_size  uint16  (bytes of MJPEG payload in this packet)
//   payload       uint8[payload_size]  (raw MJPEG data, split across packets)
//
// Receiver logic:
//   - collect packets with same (camera_id, frame_id) until packet_index 0..N-1 seen
//   - concatenate payloads in packet_index order -> MJPEG frame
//   - a frame is complete when all total_packets received
//
// ============================================================================

#define _GNU_SOURCE

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <linux/videodev2.h>

// ============================================================================
// Globals / logging
// ============================================================================

static std::atomic<bool> g_stop{false};

static void log_line(const char* level, const std::string& msg) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t   = system_clock::to_time_t(now);
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    std::fprintf(stderr, "[%s.%03ld][%s] %s\n", buf, (long)ms, level, msg.c_str());
}

#define LOG_INFO(msg) do { std::string _m=(msg); log_line("INFO", _m); } while(0)
#define LOG_WARN(msg) do { std::string _m=(msg); log_line("WARN", _m); } while(0)
#define LOG_ERR(msg)  do { std::string _m=(msg); log_line("ERR ", _m); } while(0)

// ============================================================================
// Helpers
// ============================================================================

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(delim, start);
        if (p == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

static std::string errno_str() { return std::string(std::strerror(errno)); }

// ============================================================================
// Configuration
// ============================================================================

struct CameraConfig {
    int         id     = 0;
    std::string device;
    int         width  = 1920;
    int         height = 1080;
    int         fps    = 30;
};

struct Config {
    std::string host;
    uint16_t    port                 = 5000;
    int         max_udp_payload      = 1400;              // bytes (header + payload)
    int         socket_send_buffer   = 8 * 1024 * 1024;   // SO_SNDBUF
    std::vector<CameraConfig> cameras;
};

// ============================================================================
// V4L2 camera (MMAP streaming, MJPEG)
// ============================================================================

class V4L2Camera {
public:
    explicit V4L2Camera(const CameraConfig& cfg) : cfg_(cfg) {
        try {
            open_device();
            query_caps();
            set_format();
            set_framerate();
            init_mmap();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~V4L2Camera() { cleanup(); }

    V4L2Camera(const V4L2Camera&)            = delete;
    V4L2Camera& operator=(const V4L2Camera&) = delete;

    void start() {
        if (streaming_) return;
        for (uint32_t i = 0; i < buffers_.size(); ++i) {
            v4l2_buffer buf{};
            buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index  = i;
            if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0)
                throw std::runtime_error("VIDIOC_QBUF: " + errno_str());
        }
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0)
            throw std::runtime_error("VIDIOC_STREAMON: " + errno_str());
        streaming_ = true;
    }

    void stop() {
        if (!streaming_) return;
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }

    // Grab one frame. Returns false on timeout. On true, caller MUST call release().
    bool grab(const uint8_t*& data, size_t& size, int timeout_ms = 1000) {
        if (current_has_frame_) {
            // Safety net — shouldn't happen if caller is correct
            release();
        }
        struct pollfd pfd{};
        pfd.fd     = fd_;
        pfd.events = POLLIN;

        int r;
        do { r = poll(&pfd, 1, timeout_ms); } while (r < 0 && errno == EINTR);
        if (r == 0) return false;
        if (r < 0)  throw std::runtime_error("poll: " + errno_str());
        if (!(pfd.revents & POLLIN)) return false;

        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) return false;
            throw std::runtime_error("VIDIOC_DQBUF: " + errno_str());
        }
        if (buf.index >= buffers_.size())
            throw std::runtime_error("VIDIOC_DQBUF: bad buffer index");

        current_index_     = buf.index;
        current_has_frame_ = true;
        data = static_cast<const uint8_t*>(buffers_[buf.index].start);
        size = buf.bytesused;
        return true;
    }

    void release() {
        if (!current_has_frame_) return;
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = current_index_;
        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            current_has_frame_ = false;
            throw std::runtime_error("VIDIOC_QBUF: " + errno_str());
        }
        current_has_frame_ = false;
    }

private:
    struct Buffer { void* start = nullptr; size_t length = 0; };

    void open_device() {
        fd_ = ::open(cfg_.device.c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd_ < 0)
            throw std::runtime_error("open " + cfg_.device + ": " + errno_str());
    }

    void query_caps() {
        v4l2_capability cap{};
        if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0)
            throw std::runtime_error("VIDIOC_QUERYCAP: " + errno_str());
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
            throw std::runtime_error(cfg_.device + ": not a video capture device");
        if (!(cap.capabilities & V4L2_CAP_STREAMING))
            throw std::runtime_error(cfg_.device + ": streaming not supported");
    }

    void set_format() {
        v4l2_format fmt{};
        fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = (uint32_t)cfg_.width;
        fmt.fmt.pix.height      = (uint32_t)cfg_.height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        fmt.fmt.pix.field       = V4L2_FIELD_ANY;

        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0)
            throw std::runtime_error("VIDIOC_S_FMT: " + errno_str());
        if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
            char fourcc[5] = {0};
            std::memcpy(fourcc, &fmt.fmt.pix.pixelformat, 4);
            throw std::runtime_error(std::string("MJPEG not supported, got '") + fourcc + "'");
        }
        actual_width_  = (int)fmt.fmt.pix.width;
        actual_height_ = (int)fmt.fmt.pix.height;
    }

    void set_framerate() {
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = (uint32_t)cfg_.fps;
        if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
            LOG_WARN("cam " + std::to_string(cfg_.id) +
                     ": VIDIOC_S_PARM failed (" + errno_str() + "), using driver default fps");
        }
    }

    void init_mmap() {
        v4l2_requestbuffers req{};
        req.count  = 4;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
            throw std::runtime_error("VIDIOC_REQBUFS: " + errno_str());
        if (req.count < 2)
            throw std::runtime_error("insufficient buffer memory");

        buffers_.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index  = i;
            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0)
                throw std::runtime_error("VIDIOC_QUERYBUF: " + errno_str());
            buffers_[i].length = buf.length;
            buffers_[i].start  = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd_, buf.m.offset);
            if (buffers_[i].start == MAP_FAILED)
                throw std::runtime_error("mmap: " + errno_str());
        }
    }

    void cleanup() noexcept {
        if (fd_ >= 0) {
            try { stop(); } catch (...) {}
            for (auto& b : buffers_) {
                if (b.start && b.start != MAP_FAILED) munmap(b.start, b.length);
            }
            buffers_.clear();
            ::close(fd_);
            fd_ = -1;
        }
    }

    CameraConfig        cfg_;
    int                 fd_            = -1;
    std::vector<Buffer> buffers_;
    int                 actual_width_  = 0;
    int                 actual_height_ = 0;
    bool                streaming_     = false;
    uint32_t            current_index_ = 0;
    bool                current_has_frame_ = false;

public:
    int actual_width()  const { return actual_width_;  }
    int actual_height() const { return actual_height_; }
};

// ============================================================================
// UDP sender
// ============================================================================

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

static constexpr uint32_t RCAM_MAGIC = 0x4D414352u; // "RCAM" LE
static_assert(sizeof(FramePacketHeader) == 16, "packet header must be 16 bytes");

class UdpFrameSender {
public:
    UdpFrameSender(const std::string& host, uint16_t port,
                   int max_udp_payload, int sndbuf)
        : max_udp_(max_udp_payload)
    {
        if (max_udp_ <= (int)sizeof(FramePacketHeader) + 8) {
            throw std::runtime_error("max_udp_payload too small");
        }
        max_payload_ = (size_t)max_udp_ - sizeof(FramePacketHeader);

        try {
            sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (sock_ < 0)
                throw std::runtime_error("socket: " + errno_str());

            int sz = sndbuf;
            if (setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz)) < 0) {
                LOG_WARN("SO_SNDBUF failed: " + errno_str());
            }

            std::memset(&dest_, 0, sizeof(dest_));
            dest_.sin_family = AF_INET;
            dest_.sin_port   = htons(port);
            if (inet_pton(AF_INET, host.c_str(), &dest_.sin_addr) != 1)
                throw std::runtime_error("invalid host address: " + host);
        } catch (...) {
            if (sock_ >= 0) { ::close(sock_); sock_ = -1; }
            throw;
        }
    }

    ~UdpFrameSender() { if (sock_ >= 0) ::close(sock_); }

    UdpFrameSender(const UdpFrameSender&)            = delete;
    UdpFrameSender& operator=(const UdpFrameSender&) = delete;

    // Returns number of packets actually sent (0 = frame dropped).
    // Never throws.
    uint32_t send_frame(uint16_t camera_id, const uint8_t* data, size_t size) noexcept {
        if (size == 0) return 0;

        uint32_t frame_id = ++frame_counter_;

        uint64_t total64 = (size + max_payload_ - 1) / max_payload_;
        if (total64 == 0 || total64 > 0xFFFFu) {
            LOG_WARN("frame too large to send: " + std::to_string(size) + " bytes");
            return 0;
        }
        uint16_t total_packets = (uint16_t)total64;

        // Layout: [hdr][payload][hdr][payload]...
        size_t total_bytes = size + (size_t)total_packets * sizeof(FramePacketHeader);
        try {
            if (packet_buffer_.size() < total_bytes) packet_buffer_.resize(total_bytes);
            if (iov_.size()  < total_packets)        iov_.resize(total_packets);
            if (msgs_.size() < total_packets)        msgs_.resize(total_packets);
        } catch (const std::bad_alloc&) {
            LOG_ERR("send buffer alloc failed");
            return 0;
        }

        size_t write_pos = 0;
        size_t read_pos  = 0;
        for (uint16_t i = 0; i < total_packets; ++i) {
            size_t payload = std::min<size_t>(max_payload_, size - read_pos);

            FramePacketHeader hdr;
            hdr.magic         = RCAM_MAGIC;
            hdr.camera_id     = camera_id;
            hdr.total_packets = total_packets;
            hdr.frame_id      = frame_id;
            hdr.packet_index  = i;
            hdr.payload_size  = (uint16_t)payload;

            std::memcpy(packet_buffer_.data() + write_pos, &hdr, sizeof(hdr));
            iov_[i].iov_base = packet_buffer_.data() + write_pos;
            iov_[i].iov_len  = sizeof(hdr) + payload;
            write_pos += sizeof(hdr);

            std::memcpy(packet_buffer_.data() + write_pos, data + read_pos, payload);
            write_pos += payload;
            read_pos  += payload;

            std::memset(&msgs_[i], 0, sizeof(msgs_[i]));
            msgs_[i].msg_hdr.msg_name    = &dest_;
            msgs_[i].msg_hdr.msg_namelen = sizeof(dest_);
            msgs_[i].msg_hdr.msg_iov     = &iov_[i];
            msgs_[i].msg_hdr.msg_iovlen  = 1;
        }

        // Send in batches (sendmmsg).
        uint32_t sent = 0;
        const uint32_t batch = 64;
        while (sent < total_packets && !g_stop.load(std::memory_order_relaxed)) {
            uint32_t n_batch = std::min<uint32_t>(batch, total_packets - sent);
            int n = ::sendmmsg(sock_, &msgs_[sent], n_batch, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                LOG_WARN("sendmmsg: " + errno_str());
                break;
            }
            if (n == 0) break;
            sent += (uint32_t)n;
        }
        bytes_sent_.fetch_add(size, std::memory_order_relaxed);
        return sent;
    }

    size_t max_payload() const { return max_payload_; }

    uint64_t frames_sent() const { return frame_counter_.load(std::memory_order_relaxed); }
    uint64_t bytes_sent()  const { return bytes_sent_.load(std::memory_order_relaxed); }

private:
    int          sock_        = -1;
    sockaddr_in  dest_{};
    int          max_udp_     = 1400;
    size_t       max_payload_ = 0;

    std::atomic<uint32_t> frame_counter_{0};
    std::atomic<uint64_t> bytes_sent_{0};

    std::vector<uint8_t>  packet_buffer_;
    std::vector<iovec>    iov_;
    std::vector<mmsghdr>  msgs_;
};

// ============================================================================
// Per-camera worker thread
// ============================================================================

static void camera_thread(const CameraConfig& cfg,
                          const std::string& host,
                          uint16_t port,
                          int max_udp,
                          int sndbuf)
{
    const std::string tag = "[cam " + std::to_string(cfg.id) + " " + cfg.device + "] ";

    std::unique_ptr<V4L2Camera>     cam;
    std::unique_ptr<UdpFrameSender> sender;

    try {
        cam    = std::make_unique<V4L2Camera>(cfg);
        sender = std::make_unique<UdpFrameSender>(host, port, max_udp, sndbuf);
        cam->start();
    } catch (const std::exception& e) {
        LOG_ERR(tag + "init failed: " + e.what());
        return;
    }

    LOG_INFO(tag + "streaming " +
             std::to_string(cam->actual_width()) + "x" + std::to_string(cam->actual_height()) +
             " MJPEG -> " + host + ":" + std::to_string(port));

    uint64_t frame_count     = 0;
    uint64_t dropped_packets = 0;
    auto     last_stats      = std::chrono::steady_clock::now();

    const uint8_t* data = nullptr;
    size_t         size = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        try {
            if (!cam->grab(data, size, 1000)) {
                continue; // timeout — нормально
            }

            uint32_t sent = sender->send_frame((uint16_t)cfg.id, data, size);

            // учесть потери (не должно случаться при нормальной сети)
            size_t max_payload  = sender->max_payload();
            uint64_t expected_pkts = (size + max_payload - 1) / max_payload;
            if (sent < expected_pkts) dropped_packets += (expected_pkts - sent);

            cam->release();
            ++frame_count;
        } catch (const std::exception& e) {
            LOG_ERR(tag + "capture error: " + e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // Периодический отчёт
        auto now = std::chrono::steady_clock::now();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats).count();
        if (ms >= 5000) {
            double fps = frame_count * 1000.0 / (double)ms;
            LOG_INFO(tag + "stats: " + std::to_string(frame_count) + " frames, " +
                     std::to_string((int)(fps + 0.5)) + " fps, dropped_pkts=" +
                     std::to_string(dropped_packets) +
                     ", total_sent_MB=" + std::to_string(sender->bytes_sent() / (1024*1024)));
            frame_count     = 0;
            dropped_packets = 0;
            last_stats      = now;
        }
    }

    try { cam->stop(); } catch (...) {}
    LOG_INFO(tag + "stopped");
}

// ============================================================================
// CLI
// ============================================================================

static void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

static void print_usage(const char* prog) {
    std::cerr
        << "Usage:\n"
        << "  " << prog << " --host <ip> [--port <p>] --camera <id>:<dev>[:<w>:<h>:<fps>] [--camera ...]\n"
        << "\n"
        << "Options:\n"
        << "  --host <ip>       destination IPv4 address (required)\n"
        << "  --port <p>        destination UDP port (default 5000)\n"
        << "  --camera <spec>   id:device[:width:height:fps]  (defaults 1920:1080:30)\n"
        << "                    may be given multiple times (one per camera)\n"
        << "  --max-udp <n>     max UDP payload incl. 16-byte header (default 1400)\n"
        << "                    use ~8900 with jumbo frames on your LAN if supported\n"
        << "  --sndbuf <n>      SO_SNDBUF bytes (default 8388608)\n"
        << "  -h, --help        show this message\n";
}

static CameraConfig parse_camera_spec(const std::string& spec) {
    auto parts = split(spec, ':');
    if (parts.size() != 2 && parts.size() != 5)
        throw std::runtime_error("bad --camera spec: " + spec);

    CameraConfig c;
    long id = std::stol(parts[0]);
    if (id < 0 || id > 65535) throw std::runtime_error("camera id out of range (0..65535)");
    c.id     = (int)id;
    c.device = parts[1];
    if (c.device.empty()) throw std::runtime_error("empty device path");

    if (parts.size() == 5) {
        c.width  = std::stoi(parts[2]);
        c.height = std::stoi(parts[3]);
        c.fps    = std::stoi(parts[4]);
    }
    if (c.width <= 0 || c.height <= 0 || c.fps <= 0)
        throw std::runtime_error("bad width/height/fps");
    if (c.fps > 240) throw std::runtime_error("fps too high");
    return c;
}

int main(int argc, char** argv) {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    Config cfg;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto need = [&](const char* name) -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
                return argv[++i];
            };
            if      (a == "--host")    cfg.host = need("--host");
            else if (a == "--port") {
                long p = std::stol(need("--port"));
                if (p <= 0 || p > 65535) throw std::runtime_error("port out of range");
                cfg.port = (uint16_t)p;
            }
            else if (a == "--camera")  cfg.cameras.push_back(parse_camera_spec(need("--camera")));
            else if (a == "--max-udp") cfg.max_udp_payload    = std::stoi(need("--max-udp"));
            else if (a == "--sndbuf")  cfg.socket_send_buffer = std::stoi(need("--sndbuf"));
            else if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
            else throw std::runtime_error("unknown argument: " + a);
        }
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n";
        print_usage(argv[0]);
        return 2;
    }

    if (cfg.host.empty())       { std::cerr << "Error: --host is required\n";       print_usage(argv[0]); return 2; }
    if (cfg.cameras.empty())    { std::cerr << "Error: at least one --camera\n";    print_usage(argv[0]); return 2; }
    if (cfg.max_udp_payload <= (int)sizeof(FramePacketHeader) + 8) {
        std::cerr << "Error: --max-udp too small\n"; return 2;
    }

    // Duplicate id check
    {
        std::vector<int> ids;
        ids.reserve(cfg.cameras.size());
        for (auto& c : cfg.cameras) ids.push_back(c.id);
        std::sort(ids.begin(), ids.end());
        for (size_t i = 1; i < ids.size(); ++i)
            if (ids[i] == ids[i-1]) { std::cerr << "Error: duplicate camera id " << ids[i] << "\n"; return 2; }
    }

    LOG_INFO("raspiCam starting: host=" + cfg.host + " port=" + std::to_string(cfg.port) +
             " cameras=" + std::to_string(cfg.cameras.size()));

    std::vector<std::thread> threads;
    threads.reserve(cfg.cameras.size());
    for (const auto& c : cfg.cameras) {
        try {
            threads.emplace_back(camera_thread, std::cref(c), std::cref(cfg.host),
                                 cfg.port, cfg.max_udp_payload, cfg.socket_send_buffer);
        } catch (const std::exception& e) {
            LOG_ERR("cannot start thread for camera " + std::to_string(c.id) + ": " + e.what());
        }
    }

    for (auto& t : threads) if (t.joinable()) t.join();

    LOG_INFO("raspiCam shutdown complete");
    return 0;
}