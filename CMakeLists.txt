cmake_minimum_required(VERSION 3.10)
project(pi_camera_sender)

set(CMAKE_CXX_STANDARD 14)

find_package(OpenCV REQUIRED)
find_package(cppzmq REQUIRED)

add_executable(
        sender src/main.cpp
        src/send_frame.cpp
                )

target_include_directories(sender PRIVATE src)
target_link_libraries(sender PRIVATE ${OpenCV_LIBS} cppzmq)