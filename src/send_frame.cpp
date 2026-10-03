#include "send_frame.hpp"
#include <cstring>

ZmqSender::ZmqSender(const std::string& bind_address)
    : context_(1),
      socket_(context_, zmq::socket_type::pub) {
    socket_.bind(bind_address);
}

bool ZmqSender::send(const void* data, size_t size) {
    zmq::message_t msg(size);
    std::memcpy(msg.data(), data, size);
    auto res = socket_.send(msg, zmq::send_flags::none);
    return res.has_value();
}