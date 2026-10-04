#pragma once

#include <cstdint>
#include <string>
#include <arpa/inet.h>

class FrameSender {
public:
    FrameSender();
    ~FrameSender();

    // Подключиться к серверу (MacBook)
    bool connect(const std::string& host, uint16_t port);

    // Отправить кадр: 4 байта длины (network order) + данные
    bool sendFrame(const uint8_t* data, size_t size);

    void close();

    bool isConnected() const { return sock_ >= 0; }

private:
    int sock_ = -1;
    std::string host_;
    uint16_t port_ = 0;

    bool reconnect();
};