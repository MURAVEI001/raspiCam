#include "CameraCapture.hpp"
#include "FrameSender.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

static std::atomic<bool> g_running{true};

static void signalHandler(int) { g_running = false; }

struct CameraJob {
    std::string name;
    std::string device;
    std::string server_ip;
    uint16_t    server_port;
};

static void cameraWorker(CameraJob job) {
    CameraCapture camera;
    if (!camera.open(job.device, 1920, 1080, 30)) {
        fprintf(stderr, "[%s] не удалось открыть %s\n",
                job.name.c_str(), job.device.c_str());
        return;
    }

    FrameSender sender;
    if (!sender.connect(job.server_ip, job.server_port, job.name)) {
        fprintf(stderr, "[%s] не удалось подключиться к %s:%u\n",
                job.name.c_str(), job.server_ip.c_str(), job.server_port);
        return;
    }

    printf("[%s] запущена (%s)\n", job.name.c_str(), job.device.c_str());

    while (g_running) {
        const uint8_t* data = nullptr;
        size_t size = 0;

        if (!camera.capture(&data, &size)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (size == 0) continue;

        if (!sender.sendFrame(data, size)) {
            fprintf(stderr, "[%s] ошибка отправки, переподключение...\n",
                    job.name.c_str());
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!sender.connect(job.server_ip, job.server_port, job.name)) {
                fprintf(stderr, "[%s] переподключение не удалось, стоп.\n",
                        job.name.c_str());
                break;
            }
        }
    }

    camera.close();
    sender.close();
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <server_ip> <server_port> <name>=<device> [<name>=<device> ...]\n",
            argv[0]);
        fprintf(stderr,
            "Example (1 camera):    %s 192.168.2.1 9000 front=/dev/video0\n",
            argv[0]);
        fprintf(stderr,
            "Example (2 cameras):   %s 192.168.2.1 9000 left=/dev/video0 right=/dev/video2\n",
            argv[0]);
        return 1;
    }

    std::string server_ip   = argv[1];
    uint16_t    server_port = static_cast<uint16_t>(std::atoi(argv[2]));

    std::vector<CameraJob> jobs;
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        auto eq = arg.find('=');
        if (eq == std::string::npos || eq == 0 || eq == arg.size() - 1) {
            fprintf(stderr, "Неверный аргумент: '%s' (ожидается name=device)\n",
                    arg.c_str());
            return 1;
        }
        CameraJob job;
        job.name        = arg.substr(0, eq);
        job.device      = arg.substr(eq + 1);
        job.server_ip   = server_ip;
        job.server_port = server_port;
        jobs.push_back(std::move(job));
    }

    if (jobs.empty()) {
        fprintf(stderr, "Не задано ни одной камеры\n");
        return 1;
    }

    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);

    printf("raspiCam: запускаю %zu камер(ы) -> %s:%u\n",
           jobs.size(), server_ip.c_str(), server_port);

    std::vector<std::thread> threads;
    threads.reserve(jobs.size());
    for (auto& job : jobs) {
        threads.emplace_back(cameraWorker, job);
    }
    for (auto& t : threads) t.join();

    printf("raspiCam завершён.\n");
    return 0;
}