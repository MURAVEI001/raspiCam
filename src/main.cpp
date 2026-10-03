#include "send_frame.hpp"
#include <iostream>

int main(int argc, char** argv) {
    // В идеале — читать из config.yaml
    std::string bind_addr = "tcp://*:5555";
    int camera_id = (argc > 1) ? std::atoi(argv[1]) : 0;
    
    cv::VideoCapture cap(camera_id);
    if (!cap.isOpened()) { std::cerr << "Камера не открылась\n"; return -1; }
    
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
    
    FrameSender sender(bind_addr);
    cv::Mat frame;
    std::cout << "Отправляю кадры на " << bind_addr << "\n";
    
// В цикле отправителя
auto t0 = std::chrono::steady_clock::now();
int frames = 0;
while (true) {
    cap >> frame;
    sender.sendFrame(frame);
    if (++frames % 30 == 0) {
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::cout << "FPS: " << 30000.0 / dt << std::endl;
        t0 = std::chrono::steady_clock::now();
    }
}
}