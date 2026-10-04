#include "FrameSender.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cerrno>

FrameSender::FrameSender() = default;

FrameSender::~FrameSender() {
    close();
}

bool FrameSender::connect(const std::string& host, uint16_t port) {
    close();
    host_ = host;
    port_ = port;
    return reconnect();
}

bool FrameSender::reconnect() {
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
        perror("FrameSender::reconnect: socket");
        return false;
    }

    // Отключаем алгоритм Нагла для минимизации задержки
    int flag = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Увеличиваем размер буфера отправки (для больших кадров)
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port_);
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        fprintf(stderr, "FrameSender: неверный адрес %s\n", host_.c_str());
        close();
        return false;
    }

    if (::connect(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("FrameSender::reconnect: connect");
        close();
        return false;
    }

    return true;
}

bool FrameSender::sendFrame(const uint8_t* data, size_t size) {
    if (sock_ < 0) {
        if (!reconnect()) return false;
    }

    // Заголовок: длина кадра в network byte order
    uint32_t len = htonl(static_cast<uint32_t>(size));

    // Отправка заголовка
    ssize_t sent = ::send(sock_, &len, sizeof(len), MSG_NOSIGNAL);
    if (sent != sizeof(len)) {
        perror("FrameSender::sendFrame: send header");
        close();
        return false;
    }

    // Отправка данных (может потребоваться несколько вызовов)
    size_t total = 0;
    while (total < size) {
        sent = ::send(sock_, data + total, size - total, MSG_NOSIGNAL);
        if (sent <= 0) {
            perror("FrameSender::sendFrame: send data");
            close();
            return false;
        }
        total += sent;
    }

    return true;
}

void FrameSender::close() {
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}