cmake_minimum_required(VERSION 3.15)

project(coro CXX)

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

# test
option(CORO_BUILD_TEST "" OFF)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(CORO_BUILD_TEST ON)
endif ()

if (CORO_BUILD_TEST)
    option(CORO_ENABLE_SANITIZE_ADDRESS "" OFF)
    option(CORO_ENABLE_SANITIZE_THREAD "" OFF)

    if (CORO_ENABLE_SANITIZE_ADDRESS)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
        set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
    endif ()
    if (CORO_ENABLE_SANITIZE_THREAD)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=thread")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
    endif ()

    file(GLOB SRCS test/main.cpp)
    link_libraries(${PROJECT_NAME})

    # test
    set(TARGET_NAME ${PROJECT_NAME}_test)
    add_executable(${TARGET_NAME} ${SRCS})

    # test verbose
    set(TARGET_NAME ${PROJECT_NAME}_test_verbose)
    add_executable(${TARGET_NAME} ${SRCS})
    target_compile_definitions(${TARGET_NAME} PUBLIC -DCORO_DEBUG_LEAK_LOG=LOG -DCORO_DEBUG_LIFECYCLE=LOG)

    # test mutex
    set(TARGET_NAME ${PROJECT_NAME}_test_mutex)
    add_executable(${TARGET_NAME} test/main_mutex.cpp)

    # test channel
    set(TARGET_NAME ${PROJECT_NAME}_test_channel)
    add_executable(${TARGET_NAME} test/main_channel.cpp)
endif ()
