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
    option(CORO_DISABLE_EXCEPTION "" OFF)
    option(CORO_TEST_RUNNER_VERY_SLOW "" OFF)
    # github actions runner of macOS is very slow
    if ($ENV{GITHUB_ACTIONS} AND $ENV{RUNNER_OS} STREQUAL "macOS")
        set(CORO_TEST_RUNNER_VERY_SLOW ON)
        add_definitions(-DCORO_TEST_RUNNER_VERY_SLOW)
    endif ()

    if (CORO_ENABLE_SANITIZE_ADDRESS)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
        set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
    endif ()
    if (CORO_ENABLE_SANITIZE_THREAD)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=thread")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
    endif ()
    if (CORO_DISABLE_EXCEPTION)
        if (MSVC)
            # MSVC
            add_compile_options(/EHs-c-)
            add_definitions(-D_HAS_EXCEPTIONS=0)
        else ()
            # GCC/Clang
            add_compile_options(-fno-exceptions)
        endif ()
        add_definitions(-DCORO_DISABLE_EXCEPTION)
    endif ()

    include_directories(test/detail)

    file(GLOB SRCS test/coro_task.cpp)
    link_libraries(${PROJECT_NAME})

    # test task
    set(TARGET_NAME ${PROJECT_NAME}_task)
    add_executable(${TARGET_NAME} ${SRCS})

    # test task verbose
    set(TARGET_NAME ${PROJECT_NAME}_task_verbose)
    add_executable(${TARGET_NAME} ${SRCS})
    target_compile_definitions(${TARGET_NAME} PUBLIC -DCORO_DEBUG_LEAK_LOG=LOG -DCORO_DEBUG_LIFECYCLE=LOG)

    # test mutex
    set(TARGET_NAME ${PROJECT_NAME}_mutex)
    add_executable(${TARGET_NAME} test/coro_mutex.cpp)

    # test condition_variable
    set(TARGET_NAME ${PROJECT_NAME}_condition_variable)
    add_executable(${TARGET_NAME} test/coro_condition_variable.cpp)

    # test condition_variable multi-threaded
    set(TARGET_NAME ${PROJECT_NAME}_condition_variable_mt)
    add_executable(${TARGET_NAME} test/coro_condition_variable_mt.cpp)

    # test channel
    set(TARGET_NAME ${PROJECT_NAME}_channel)
    add_executable(${TARGET_NAME} test/coro_channel.cpp)

    # test when
    set(TARGET_NAME ${PROJECT_NAME}_when)
    add_executable(${TARGET_NAME} test/coro_when.cpp)

    # test wait_group
    set(TARGET_NAME ${PROJECT_NAME}_wait_group)
    add_executable(${TARGET_NAME} test/coro_wait_group.cpp)

    # test wait_group multi-threaded
    set(TARGET_NAME ${PROJECT_NAME}_wait_group_mt)
    add_executable(${TARGET_NAME} test/coro_wait_group_mt.cpp)

    # test latch
    set(TARGET_NAME ${PROJECT_NAME}_latch)
    add_executable(${TARGET_NAME} test/coro_latch.cpp)

    # test multi thread
    set(TARGET_NAME ${PROJECT_NAME}_multi_thread)
    add_executable(${TARGET_NAME} test/coro_multi_thread.cpp)

    # test multi thread(use single thread)
    set(TARGET_NAME ${PROJECT_NAME}_multi_thread_st)
    add_executable(${TARGET_NAME} test/coro_multi_thread.cpp)
    target_compile_definitions(${TARGET_NAME} PRIVATE -DCORO_USE_SINGLE_THREAD)
endif ()
