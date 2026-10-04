cmake_minimum_required(VERSION 3.16)
project(rpi_mjpeg_streamer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -pipe -march=armv8-a+crc -mtune=cortex-a53 -flto")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-flto -Wl,-O1")

add_executable(mjpeg_streamer
    src/main.cpp
    src/v4l2_capture.cpp
    src/tcp_sender.cpp
    src/config.cpp
)

target_include_directories(mjpeg_streamer PRIVATE src)

# Для линковки с pthread (нужно для std::thread)
find_package(Threads REQUIRED)
target_link_libraries(mjpeg_streamer PRIVATE Threads::Threads)