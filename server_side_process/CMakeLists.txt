cmake_minimum_required(VERSION 3.16)
project(yolo_video_detector LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED COMPONENTS core dnn imgproc imgcodecs objdetect)

add_executable(yolo_video_detector main.cpp)
target_link_libraries(yolo_video_detector PRIVATE ${OpenCV_LIBS})
target_compile_options(yolo_video_detector PRIVATE -Wall -Wextra -Wpedantic)
