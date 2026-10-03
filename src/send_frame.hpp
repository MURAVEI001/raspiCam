#pragma once
#include <zmq.hpp>
#include <string>
#include <cstddef>

class ZmqSender {
public:
    explicit ZmqSender(const std::string& bind_address);
    // Отправляет байты как одно ZMQ-сообщение
    bool send(const void* data, size_t size);
private:
    zmq::context_t context_;
    zmq::socket_t  socket_;
};