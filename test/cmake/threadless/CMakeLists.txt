cmake_minimum_required(VERSION 3.15)
project(coro_threadless_consumer CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../warnings.cmake")

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../.." coro)
get_target_property(threadless_link_dependencies coro INTERFACE_LINK_LIBRARIES)
if (threadless_link_dependencies)
    message(FATAL_ERROR "Threadless components must not require a native thread library")
endif ()

add_library(threadless_consumer OBJECT ../threadless.cpp)
target_link_libraries(threadless_consumer PRIVATE coro)
target_compile_definitions(threadless_consumer PRIVATE CORO_TEST_NO_NATIVE_MUTEX)
