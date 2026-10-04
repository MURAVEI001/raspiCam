#pragma once

#include <cstdint>
#include <string>

class FrameSender {
public:
    FrameSender();
    ~FrameSender();

    // Подключиться и представиться именем камеры (hello).
    bool connect(const std::string& host, uint16_t port,
                 const std::string& camera_name);

    bool sendFrame(const uint8_t* data, size_t size);
    void close();

    bool isConnected() const { return sock_ >= 0; }

private:
    int         sock_ = -1;
    std::string host_;
    uint16_t    port_ = 0;
    std::string name_;

    bool reconnect();
};