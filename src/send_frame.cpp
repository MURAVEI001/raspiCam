#include "send_frame.hpp"

FrameSender::FrameSender(const std::string& bind_address, int jpeg_quality)
    : context_(1), socket_(context_, zmq::socket_type::pub), jpeg_quality_(jpeg_quality) {
    socket_.bind(bind_address);
    params_ = {cv::IMWRITE_JPEG_QUALITY, jpeg_quality_};
}

bool FrameSender::sendFrame(const cv::Mat& frame) {
    if (frame.empty()) return false;
    std::vector<uchar> buffer;
    if (!cv::imencode(".jpg", frame, buffer, params_)) return false;
    zmq::message_t msg(buffer.size());
    memcpy(msg.data(), buffer.data(), buffer.size());
    socket_.send(msg, zmq::send_flags::none);
    return true;
}