#include "CameraCapture.hpp"
#include "FrameSender.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

static std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <video_device>\n", argv[0]);
        fprintf(stderr, "Example: %s 192.168.1.100 9000 /dev/video0\n", argv[0]);
        return 1;
    }

    const char* server_ip   = argv[1];
    uint16_t    server_port = static_cast<uint16_t>(std::atoi(argv[2]));
    const char* device      = argv[3];

    // Настройка обработки сигналов
    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);

    CameraCapture camera;
    if (!camera.open(device, 1920, 1080, 30)) {
        fprintf(stderr, "Не удалось открыть камеру %s\n", device);
        return 1;
    }

    FrameSender sender;
    if (!sender.connect(server_ip, server_port)) {
        fprintf(stderr, "Не удалось подключиться к %s:%u\n", server_ip, server_port);
        return 1;
    }

    printf("raspiCam запущен. Отправка кадров на %s:%u\n", server_ip, server_port);

    uint64_t frame_count = 0;
    auto last_report = std::chrono::steady_clock::now();

    while (g_running) {
        const uint8_t* data = nullptr;
        size_t size = 0;

        if (!camera.capture(&data, &size)) {
            // Нет кадра — короткая пауза, чтобы не жечь CPU
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if (size == 0) continue;

        if (!sender.sendFrame(data, size)) {
            fprintf(stderr, "Ошибка отправки кадра, попытка переподключения...\n");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!sender.connect(server_ip, server_port)) {
                fprintf(stderr, "Переподключение не удалось, выход.\n");
                break;
            }
        }

        ++frame_count;

        // Отчёт раз в секунду
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report).count() >= 1) {
            printf("Отправлено кадров: %llu\n", (unsigned long long)frame_count);
            last_report = now;
        }
    }

    camera.close();
    sender.close();
    printf("raspiCam завершён.\n");
    return 0;
}