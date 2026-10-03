cmake_minimum_required(VERSION 3.10)
project(raspiCam CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq)

add_executable(sender
    src/main.cpp
    src/v4l2_capture.cpp
    src/send_frame.cpp
)
target_include_directories(sender PRIVATE src)
target_link_libraries(sender PRIVATE PkgConfig::ZMQ)