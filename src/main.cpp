#include "v4l2_capture.hpp"
#include "send_frame.hpp"
#include <iostream>
#include <chrono>

int main() {
    try {
        V4L2Capture capture("/dev/video0", 1920, 1080);
        ZmqSender sender("tcp://*:5555");

        std::cout << "Отправляю кадры на tcp://*:5555\n";

        int frames = 0;
        auto t0 = std::chrono::steady_clock::now();

        while (true) {
            const void* data;
            size_t size;
            if (!capture.waitFrame(&data, &size, 1000)) continue;

            sender.send(data, size);
            capture.releaseFrame();

            if (++frames % 30 == 0) {
                auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                std::cout << "FPS: " << 30000.0 / dt << std::endl;
                t0 = std::chrono::steady_clock::now();
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}