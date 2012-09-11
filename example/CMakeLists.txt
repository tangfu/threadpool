cmake_minimum_required(VERSION 2.8)
project(threadpool-example)

find_package(threadpool REQUIRED)

add_definitions("-g -Wall")
add_executable(example example.c)
target_link_libraries(example pthread threadpool)
