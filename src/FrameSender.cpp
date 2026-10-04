#include "FrameSender.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cerrno>

FrameSender::FrameSender() = default;
FrameSender::~FrameSender() { close(); }

bool FrameSender::connect(const std::string& host, uint16_t port,
                          const std::string& camera_name) {
    close();
    host_ = host;
    port_ = port;
    name_ = camera_name;
    return reconnect();
}

bool FrameSender::reconnect() {
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
        perror("FrameSender::reconnect: socket");
        return false;
    }

    int flag = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

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

    if (::connect(sock_, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        perror("FrameSender::reconnect: connect");
        close();
        return false;
    }

    // --- HELLO: 2 байта длины имени + имя ---
    if (name_.empty() || name_.size() > 128) {
        fprintf(stderr, "FrameSender: некорректное имя камеры (len=%zu)\n",
                name_.size());
        close();
        return false;
    }
    uint16_t name_len_net = htons(static_cast<uint16_t>(name_.size()));
    if (::send(sock_, &name_len_net, sizeof(name_len_net), MSG_NOSIGNAL)
            != sizeof(name_len_net)) {
        perror("FrameSender::reconnect: send hello len");
        close();
        return false;
    }
    size_t sent = 0;
    while (sent < name_.size()) {
        ssize_t n = ::send(sock_, name_.data() + sent,
                           name_.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            perror("FrameSender::reconnect: send hello name");
            close();
            return false;
        }
        sent += static_cast<size_t>(n);
    }

    return true;
}

bool FrameSender::sendFrame(const uint8_t* data, size_t size) {
    if (sock_ < 0) {
        if (!reconnect()) return false;
    }

    uint32_t len = htonl(static_cast<uint32_t>(size));

    ssize_t sent = ::send(sock_, &len, sizeof(len), MSG_NOSIGNAL);
    if (sent != sizeof(len)) {
        perror("FrameSender::sendFrame: send header");
        close();
        return false;
    }

    size_t total = 0;
    while (total < size) {
        sent = ::send(sock_, data + total, size - total, MSG_NOSIGNAL);
        if (sent <= 0) {
            perror("FrameSender::sendFrame: send data");
            close();
            return false;
        }
        total += static_cast<size_t>(sent);
    }
    return true;
}

void FrameSender::close() {
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}