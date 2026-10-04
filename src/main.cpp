/*
 * raspiCam — захват MJPG с камеры на Raspberry Pi и отправка кадров по UDP.
 *
 * Сборка:
 *     mkdir -p build && cd build
 *     cmake ..
 *     make -j$(nproc)
 *
 * Запуск:
 *     ./raspiCam <camera_id> <host_ip> <port> [device] [width] [height] [fps]
 *
 * Примеры:
 *     ./raspiCam 3 192.168.1.10 5000
 *     ./raspiCam 7 192.168.1.10 6000 1 640 480 15
 *
 * Формат UDP-пакета (все многобайтовые поля — сетевой порядок байт):
 *     magic      : uint32 = 0x52415350 ("RASP")
 *     camera_id  : uint16
 *     frame_id   : uint32
 *     total_size : uint32
 *     chunk_off  : uint32
 *     chunk_len  : uint32
 *     payload    : chunk_len байт JPEG
 */

#include <opencv2/opencv.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kMagic         = 0x52415350; // "RASP"
constexpr int      kMaxUdpPayload = 1400;       // безопасно для Ethernet MTU 1500
constexpr int      kDefaultFps    = 30;
constexpr int      kDefaultWidth  = 1280;
constexpr int      kDefaultHeight = 720;

std::atomic<bool> g_running{true};

void onSignal(int) {
    g_running = false;
}

// ---------------------------------------------------------------------------
// RAII-обёртка для UDP-сокета.
// ---------------------------------------------------------------------------
class UdpSender {
public:
    UdpSender(const std::string& host, uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error(
                std::string("socket() failed: ") + std::strerror(errno));
        }

        std::memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_port   = htons(port);

        if (::inet_pton(AF_INET, host.c_str(), &addr_.sin_addr) != 1) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("inet_pton() failed for host: " + host);
        }
    }

    ~UdpSender() {
        if (fd_ >= 0) ::close(fd_);
    }

    UdpSender(const UdpSender&)            = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    bool send(const void* data, size_t len) {
        const ssize_t sent = ::sendto(
            fd_, data, len, 0,
            reinterpret_cast<const sockaddr*>(&addr_), sizeof(addr_));

        if (sent < 0) {
            std::cerr << "[udp] sendto() error: " << std::strerror(errno) << "\n";
            return false;
        }
        if (static_cast<size_t>(sent) != len) {
            std::cerr << "[udp] partial send: " << sent << " / " << len << "\n";
            return false;
        }
        return true;
    }

private:
    int         fd_{-1};
    sockaddr_in addr_{};
};

// ---------------------------------------------------------------------------
// Заголовок протокола. Обязательно упакован в 22 байта.
// ---------------------------------------------------------------------------
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
// Открытие камеры с проверкой всех ключевых параметров.
// ---------------------------------------------------------------------------
cv::VideoCapture openCamera(int device_id, int width, int height, int fps) {
    cv::VideoCapture cap;

    if (!cap.open(device_id, cv::CAP_V4L2)) {
        throw std::runtime_error(
            "cannot open camera /dev/video" + std::to_string(device_id));
    }

    if (!cap.isOpened()) {
        throw std::runtime_error("camera opened but isOpened() returned false");
    }

    const uint32_t fourcc = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
    cap.set(cv::CAP_PROP_FOURCC,        fourcc);
    cap.set(cv::CAP_PROP_FRAME_WIDTH,   width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT,  height);
    cap.set(cv::CAP_PROP_FPS,           fps);

    const uint32_t actual_fourcc =
        static_cast<uint32_t>(cap.get(cv::CAP_PROP_FOURCC));

    if (actual_fourcc != fourcc) {
        char got[5] = {0, 0, 0, 0, 0};
        std::memcpy(got, &actual_fourcc, 4);
        throw std::runtime_error(
            std::string("camera does not provide MJPG, got: ") + got);
    }

    const int    actual_w   = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    const int    actual_h   = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    const double actual_fps = cap.get(cv::CAP_PROP_FPS);

    std::cout << "[cam] opened device " << device_id
              << " | " << actual_w << "x" << actual_h
              << " | MJPG | fps=" << actual_fps << "\n";

    if (actual_w <= 0 || actual_h <= 0) {
        throw std::runtime_error("camera returned invalid resolution");
    }

    return cap;
}

// ---------------------------------------------------------------------------
// Отправка одного JPEG-кадра, разбитого на UDP-датаграммы.
// ---------------------------------------------------------------------------
void sendFrame(UdpSender& sender,
               uint16_t camera_id,
               uint32_t frame_id,
               const std::vector<uint8_t>& jpeg) {
    const size_t total = jpeg.size();
    if (total == 0) return;

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
        std::memcpy(packet.data() + sizeof(hdr), jpeg.data() + offset, chunk);

        if (!sender.send(packet.data(), sizeof(hdr) + chunk)) {
            std::cerr << "[cam " << camera_id << "] frame " << frame_id
                      << " chunk at offset " << offset << " failed\n";
            return; // не рвём поток из-за одной потерянной датаграммы
        }

        offset += chunk;
    }
}

// ---------------------------------------------------------------------------
// Основной цикл: читаем кадр, кодируем в JPEG, отправляем.
// ---------------------------------------------------------------------------
void captureLoop(cv::VideoCapture& cap,
                 UdpSender& sender,
                 uint16_t camera_id,
                 int fps) {
    const auto frame_interval =
        std::chrono::microseconds(1'000'000 / std::max(1, fps));
    auto next_deadline = std::chrono::steady_clock::now();

    uint32_t frame_id = 0;

    while (g_running) {
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty()) {
            std::cerr << "[cam " << camera_id << "] read() failed, retrying...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        // CAP_V4L2 отдаёт уже декодированный BGR-кадр. Кодируем обратно в JPEG.
        // Для соревнований этого достаточно; для минимальной латентности
        // можно перейти на прямой V4L2-захват с V4L2_PIX_FMT_MJPEG.
        std::vector<uint8_t> jpeg;
        const std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 85};
        if (!cv::imencode(".jpg", frame, jpeg, params)) {
            std::cerr << "[cam " << camera_id << "] imencode() failed\n";
            continue;
        }
        if (jpeg.empty()) {
            std::cerr << "[cam " << camera_id << "] empty JPEG buffer\n";
            continue;
        }

        sendFrame(sender, camera_id, frame_id++, jpeg);

        next_deadline += frame_interval;
        const auto now = std::chrono::steady_clock::now();
        if (next_deadline > now) {
            std::this_thread::sleep_until(next_deadline);
        } else {
            next_deadline = now;
        }
    }

    std::cout << "[cam " << camera_id << "] stopped, frames sent: "
              << frame_id << "\n";
}

} // namespace

// ---------------------------------------------------------------------------
// Точка входа.
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr
            << "Usage: " << argv[0]
            << " <camera_id> <host_ip> <port> [device] [width] [height] [fps]\n\n"
            << "  camera_id  : постоянный ID камеры (0..65535)\n"
            << "  host_ip    : IP ноутбука, например 192.168.1.10\n"
            << "  port       : UDP-порт, например 5000\n"
            << "  device     : номер /dev/videoN (по умолчанию 0)\n"
            << "  width      : ширина кадра (по умолчанию 1280)\n"
            << "  height     : высота кадра (по умолчанию 720)\n"
            << "  fps        : целевой FPS (по умолчанию 30)\n";
        return 1;
    }

    int camera_id = 0;
    try {
        camera_id = std::stoi(argv[1]);
    } catch (...) {
        std::cerr << "camera_id must be an integer\n";
        return 1;
    }
    if (camera_id < 0 || camera_id > 65535) {
        std::cerr << "camera_id must be in range 0..65535\n";
        return 1;
    }

    const std::string host = argv[2];

    int port = 0;
    try {
        port = std::stoi(argv[3]);
    } catch (...) {
        std::cerr << "port must be an integer\n";
        return 1;
    }
    if (port <= 0 || port > 65535) {
        std::cerr << "port must be in range 1..65535\n";
        return 1;
    }

    int device = 0, width = kDefaultWidth, height = kDefaultHeight, fps = kDefaultFps;

    try {
        if (argc > 4) device = std::stoi(argv[4]);
        if (argc > 5) width  = std::stoi(argv[5]);
        if (argc > 6) height = std::stoi(argv[6]);
        if (argc > 7) fps    = std::stoi(argv[7]);
    } catch (...) {
        std::cerr << "device/width/height/fps must be integers\n";
        return 1;
    }

    if (device < 0 || width <= 0 || height <= 0 || fps <= 0) {
        std::cerr << "device must be >= 0; width, height, fps must be positive\n";
        return 1;
    }

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        UdpSender sender(host, static_cast<uint16_t>(port));
        std::cout << "[net] UDP target: " << host << ":" << port << "\n";

        cv::VideoCapture cap = openCamera(device, width, height, fps);

        captureLoop(cap, sender, static_cast<uint16_t>(camera_id), fps);
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << "\n";
        return 1;
    }

    return 0;
}