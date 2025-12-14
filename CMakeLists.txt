cmake_minimum_required(VERSION 3.15)

project(coro CXX)

# config
option(CORO_ENABLE_SANITIZE_ADDRESS "" OFF)
option(CORO_ENABLE_SANITIZE_THREAD "" OFF)
# test
option(CORO_BUILD_TEST "" OFF)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(CORO_BUILD_TEST ON)
endif ()

set(CMAKE_CXX_STANDARD 20)
if (MSVC)
    add_compile_options(/Zc:preprocessor)
    add_compile_options(/utf-8)
    add_compile_options(-DNOMINMAX)
else ()
    add_compile_options(-Wall -Wunused-parameter)
endif ()

add_library(${PROJECT_NAME} INTERFACE)
target_include_directories(${PROJECT_NAME} INTERFACE include)

if (CORO_BUILD_TEST)
    if (CORO_ENABLE_SANITIZE_ADDRESS)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
        set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
    endif ()
    if (CORO_ENABLE_SANITIZE_THREAD)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=thread")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
    endif ()

    set(TARGET_NAME ${PROJECT_NAME}_test)
    file(GLOB SRCS test/main.cpp)
    link_libraries(${PROJECT_NAME})
    add_executable(${TARGET_NAME} ${SRCS})
endif ()
