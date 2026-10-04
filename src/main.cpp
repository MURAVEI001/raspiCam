#include "config.hpp"
#include "v4l2_capture.hpp"
#include "tcp_sender.hpp"
#include "log.hpp"

#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>

using namespace mjpeg;

static std::atomic<bool> g_stop{false};

static void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

// Проверка: кадр должен быть валидным JPEG (SOI ... EOI).
static bool is_valid_jpeg(const uint8_t* d, size_t n) {
    if (n < 4) return false;
    if (d[0] != 0xFF || d[1] != 0xD8) return false;      // SOI
    if (d[n-2] != 0xFF || d[n-1] != 0xD9) return false;  // EOI
    return true;
}

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN); // чтобы не убивало при обрыве TCP

    std::string cfg_path = "/etc/mjpeg-streamer.ini";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            cfg_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: %s [--config path]\n", argv[0]);
            return 0;
        }
    }

    Config cfg;
    load_config(cfg_path, cfg);

    LOG_INFO("config: dev=%s %dx%d@%d -> %s:%d (buffers=%d, watchdog=%dms)",
             cfg.device.c_str(), cfg.width, cfg.height, cfg.fps,
             cfg.server_host.c_str(), cfg.server_port,
             cfg.buffer_count, cfg.watchdog_ms);

    V4L2Capture cam;
    TcpSender   sender;

    // Основной цикл: если что-то падает — перезапускаем с паузой.
    while (!g_stop.load(std::memory_order_relaxed)) {
        // 1) Открываем камеру
        if (!cam.open_device(cfg.device, cfg.width, cfg.height,
                             cfg.fps, cfg.buffer_count)) {
            LOG_ERR("open camera failed, retry in %d ms", cfg.reconnect_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.reconnect_ms));
            continue;
        }

        // 2) Подключаемся к Mac
        while (!g_stop.load(std::memory_order_relaxed)) {
            if (sender.connect_to(cfg.server_host, cfg.server_port, 3000)) break;
            LOG_ERR("connect failed, retry in %d ms", cfg.reconnect_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.reconnect_ms));
        }
        if (g_stop.load()) break;

        // 3) Горячий цикл
        auto last_frame_time = std::chrono::steady_clock::now();
        uint64_t frames_sent = 0;
        uint64_t bytes_sent  = 0;
        auto stat_time = last_frame_time;

        while (!g_stop.load(std::memory_order_relaxed)) {
            auto frame = cam.capture(1000);
            if (!frame.data || frame.size == 0) {
                auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_frame_time).count();
                if (idle > cfg.watchdog_ms) {
                    LOG_ERR("watchdog: no frames for %lld ms, restarting camera",
                            static_cast<long long>(idle));
                    break;
                }
                continue;
            }

            if (!is_valid_jpeg(frame.data, frame.size)) {
                LOG_WARN("bad JPEG frame (size=%zu), dropping", frame.size);
                cam.release();
                continue;
            }

            bool ok = sender.send_frame(frame.data, frame.size);
            cam.release();

            if (!ok) {
                LOG_ERR("send_frame failed: %s", strerror(errno));
                break; // переподключимся
            }

            last_frame_time = std::chrono::steady_clock::now();
            ++frames_sent;
            bytes_sent += frame.size;

            // Раз в секунду печатаем стату
            auto now = std::chrono::steady_clock::now();
            auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - stat_time).count();
            if (since >= 1000) {
                double mbps = (bytes_sent * 8.0) / (since / 1000.0) / 1e6;
                LOG_INFO("fps=%.1f  mbps=%.2f  frames=%llu",
                         frames_sent * 1000.0 / since, mbps,
                         static_cast<unsigned long long>(frames_sent));
                frames_sent = 0;
                bytes_sent  = 0;
                stat_time   = now;
            }
        }

        cam.close_device();
        sender.close();
        if (g_stop.load()) break;

        LOG_INFO("restarting pipeline in %d ms", cfg.reconnect_ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.reconnect_ms));
    }

    LOG_INFO("shutting down");
    cam.close_device();
    sender.close();
    return 0;
}