project(threadpool)
cmake_minimum_required(VERSION 2.8.4)


add_definitions("-g -Wall")
add_subdirectory(src)


export(PACKAGE mylib)
export(TARGETS threadpool FILE threadpool-exports.cmake)
configure_file(threadpool-config.cmake.in ${CMAKE_BINARY_DIR}/threadpool-config.cmake)
