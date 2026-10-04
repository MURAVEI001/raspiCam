cmake_minimum_required(VERSION 3.10)
project(raspiCam CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Базовая оптимизация
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# Определяем архитектуру процессора
execute_process(
    COMMAND uname -m
    OUTPUT_VARIABLE ARCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "Обнаружена архитектура: ${ARCH}")

if(ARCH STREQUAL "aarch64")
    # 64-битная Raspberry Pi OS (ARMv8)
    # NEON на aarch64 обязателен, -mfpu/-mfloat-abi не нужны
    add_compile_options(-mcpu=native)
elseif(ARCH MATCHES "^armv7")
    # 32-битная Raspberry Pi OS (ARMv7)
    add_compile_options(-mcpu=native -mfpu=neon-vfpv4 -mfloat-abi=hard)
else()
    # x86_64 и прочее (например, если собираете на MacBook для тестов)
    add_compile_options(-march=native)
endif()

find_package(Threads REQUIRED)

add_executable(raspiCam
    src/main.cpp
    src/CameraCapture.cpp
    src/FrameSender.cpp
)

target_link_libraries(raspiCam PRIVATE Threads::Threads)