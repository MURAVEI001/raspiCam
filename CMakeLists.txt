cmake_minimum_required(VERSION 3.10)
project(pi_camera_sender CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Release по умолчанию + оптимизация
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Архитектурные флаги (только для 64-bit)
execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
    OUTPUT_VARIABLE TRIPLE OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(TRIPLE MATCHES "aarch64")
    add_compile_options(-mcpu=cortex-a53)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq)

add_executable(sender
    src/main.cpp
    src/v4l2_capture.cpp
    src/send_frame.cpp
)
target_include_directories(sender PRIVATE src)
target_link_libraries(sender PRIVATE PkgConfig::ZMQ pthread)