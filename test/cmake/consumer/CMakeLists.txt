cmake_minimum_required(VERSION 3.15)
project(coro_consumer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../.." coro)

if (TARGET coro_task)
    message(FATAL_ERROR "Subproject tests should be disabled by default")
endif ()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE coro)
target_include_directories(consumer PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../detail")

enable_testing()
add_test(NAME consumer_run COMMAND consumer)
