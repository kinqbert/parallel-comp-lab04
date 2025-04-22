cmake_minimum_required(VERSION 3.20)
project(parallel_comp_lab04)

set(CMAKE_CXX_STANDARD 20)

add_executable(client client.cpp)
add_executable(server server.cpp)

target_link_libraries(client ws2_32)
target_link_libraries(server ws2_32)

