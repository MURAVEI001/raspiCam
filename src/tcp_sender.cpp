#include "tcp_sender.hpp"
#include "log.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <cstdint>

namespace mjpeg {

TcpSender::~TcpSender() {
    close();
}

bool TcpSender::connect_to(const std::string& host, int port, int timeout_ms) {
    close();

    // Разрешаем и имя, и IP
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    char portstr[16];
    std::snprintf(portstr, sizeof(portstr), "%d", port);
    int gai = getaddrinfo(host.c_str(), portstr, &hints, &res);
    if (gai != 0) {
        LOG_ERR("getaddrinfo(%s): %s", host.c_str(), gai_strerror(gai));
        return false;
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        LOG_ERR("socket: %s", strerror(errno));
        freeaddrinfo(res);
        return false;
    }

    // TCP_NODELAY — критично для низкой задержки
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int sndbuf = 1 << 20; // 1 MiB
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    // Таймаут на connect
    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        LOG_ERR("connect(%s:%d): %s", host.c_str(), port, strerror(errno));
        ::close(fd);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    fd_ = fd;
    LOG_INFO("connected to %s:%d", host.c_str(), port);
    return true;
}

static bool write_all(int fd, const uint8_t* data, size_t size) {
    size_t off = 0;
    while (off < size) {
        ssize_t n = ::send(fd, data + off, size - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

bool TcpSender::send_frame(const uint8_t* data, size_t size) {
    if (fd_ < 0) return false;
    if (size == 0 || size > 8 * 1024 * 1024) {
        LOG_WARN("frame size %zu out of sane range, skipping", size);
        return false;
    }

    // Заголовок: 4 байта BE длина
    uint8_t hdr[4];
    hdr[0] = static_cast<uint8_t>((size >> 24) & 0xFF);
    hdr[1] = static_cast<uint8_t>((size >> 16) & 0xFF);
    hdr[2] = static_cast<uint8_t>((size >>  8) & 0xFF);
    hdr[3] = static_cast<uint8_t>((size      ) & 0xFF);

    if (!write_all(fd_, hdr, 4)) return false;
    if (!write_all(fd_, data, size)) return false;
    return true;
}

void TcpSender::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace mjpeg