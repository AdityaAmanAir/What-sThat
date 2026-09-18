cmake_minimum_required(VERSION 3.16)
project(message_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(message_server main.cpp http_server.cpp)
target_link_libraries(message_server PRIVATE Threads::Threads)
target_compile_options(message_server PRIVATE -Wall -Wextra -Wpedantic)
