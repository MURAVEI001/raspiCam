#include "v4l2_capture.hpp"
#include "send_frame.hpp"
#include <iostream>
#include <chrono>
#include <filesystem>
#include <stdexcept>

// Автоматически находит камеру в /dev/v4l/by-id/,
// у которой имя оканчивается на "-video-index0".
static std::string findCameraDevice() {
    namespace fs = std::filesystem;
    const std::string dir = "/dev/v4l/by-id/";

    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        const std::string suffix = "-video-index0";
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return entry.path().string();
        }
    }
    throw std::runtime_error("Не найдено устройство *-video-index0 в " + dir);
}

int main() {
    try {
        const std::string device = findCameraDevice();
        std::cout << "Использую камеру: " << device << std::endl;

        const int target_fps = 25;

        V4L2Capture capture(device, 1280, 720, target_fps);
        ZmqSender sender("tcp://*:5555");

        std::cout << "Отправляю кадры на tcp://*:5555 @" << target_fps << " FPS\n";

        // Таймер для гарантии не более target_fps отправок в секунду
        const auto frame_interval = std::chrono::milliseconds(1000 / target_fps);
        auto next_send_time = std::chrono::steady_clock::now();

        int frames = 0;
        auto t0 = std::chrono::steady_clock::now();

        while (true) {
            const void* data;
            size_t size;
            if (!capture.waitFrame(&data, &size, 1000)) continue;

            auto now = std::chrono::steady_clock::now();
            if (now >= next_send_time) {
                sender.send(data, size);
                next_send_time = now + frame_interval;

                if (++frames % target_fps == 0) {
                    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                    std::cout << "FPS: " << target_fps * 1000.0 / dt << std::endl;
                    t0 = std::chrono::steady_clock::now();
                }
            }

            capture.releaseFrame();
        }
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}