#pragma once
#include <string>

namespace mjpeg {

struct Config {
    std::string device      = "/dev/video0";  // путь к V4L2-устройству
    int         width       = 1920;           // желаемое разрешение
    int         height      = 1080;
    int         fps         = 30;
    int         buffer_count= 4;              // mmap-буферов V4L2
    int         jpeg_quality= 80;             // если камера отдаёт MJPEG — игнорируется
    std::string server_host = "192.168.1.10"; // IP MacBook
    int         server_port = 9000;
    int         reconnect_ms= 1000;           // пауза между попытками переподключения
    int         watchdog_ms = 2000;           // если нет кадра дольше — перезапуск камеры
    bool        verbose     = false;
};

// Парсит INI-подобный конфиг. Возвращает false только если файл явно задан и не открылся.
bool load_config(const std::string& path, Config& out);

} // namespace mjpeg