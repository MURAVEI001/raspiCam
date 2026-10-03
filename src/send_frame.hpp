// send_frame.hpp
#pragma once
#include <zmq.hpp>
#include <opencv2/opencv.hpp>
#include <string>

class FrameSender {
public:
    FrameSender(const std::string& bind_address, int jpeg_quality = 80);
    bool sendFrame(const cv::Mat& frame);
private:
    zmq::context_t context_;
    zmq::socket_t socket_;
    int jpeg_quality_;
    std::vector<int> params_;
};