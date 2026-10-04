cmake_minimum_required(VERSION 3.16)
project(raspiCam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)

add_executable(raspiCam src/main.cpp)

target_include_directories(raspiCam PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(raspiCam PRIVATE ${OpenCV_LIBS} pthread)

# Включаем предупреждения — полезно на соревнованиях
target_compile_options(raspiCam PRIVATE -Wall -Wextra -Wpedantic)