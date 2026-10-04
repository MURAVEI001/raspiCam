cmake_minimum_required(VERSION 3.10)
project(raspiCam CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=armv8-a -mtune=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard")

find_package(Threads REQUIRED)

add_executable(raspiCam
    src/main.cpp
    src/CameraCapture.cpp
    src/FrameSender.cpp
)

target_link_libraries(raspiCam PRIVATE Threads::Threads)

# Для Raspberry Pi 3 (armv7) флаги могут отличаться:
# -march=armv7-a -mtune=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=hard