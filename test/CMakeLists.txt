cmake_minimum_required(VERSION 2.8)
project(threadpool-test)

include_directories("/usr/include/google")
find_package(threadpool REQUIRED)
add_definitions("-g -Wall")

add_executable(test test.c)
target_link_libraries(test cmockery pthread threadpool)
