#pragma once
#include <string>
#include <cstdint>

namespace mjpeg {

// Отправляет по TCP на сервер пакеты вида:
//   [uint32 BE length][JPEG data]
// На стороне Mac это легко разобрать.
class TcpSender {
public:
    TcpSender() = default;
    ~TcpSender();

    TcpSender(const TcpSender&) = delete;
    TcpSender& operator=(const TcpSender&) = delete;

    // Подключается. При ошибке возвращает false, печатает в лог.
    bool connect_to(const std::string& host, int port, int timeout_ms);

    // Отправляет один кадр. true — успех.
    bool send_frame(const uint8_t* data, size_t size);

    void close();

    bool is_connected() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

} // namespace mjpeg