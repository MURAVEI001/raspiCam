#include "v4l2_capture.hpp"
#include "send_frame.hpp"
#include <iostream>
#include <chrono>

int main() {
    try {
        // Используем стабильный путь через by-id вместо /dev/video0
        const std::string device = "usb-Sonix_Technology_Co.__Ltd._Autodarts_DIY_Cam_SN0001-video-index0";
        const int target_fps = 25;

        V4L2Capture capture(device, 1920, 1080, target_fps);
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

                // Счётчик FPS
                if (++frames % target_fps == 0) {
                    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                    std::cout << "FPS: " << target_fps * 1000.0 / dt << std::endl;
                    t0 = std::chrono::steady_clock::now();
                }
            }
            // Если ещё рано — кадр игнорируется

            capture.releaseFrame();
        }
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}