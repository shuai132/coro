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

option(CORO_DISABLE_EXCEPTION "Disable C++ exceptions" OFF)
option(CORO_ENABLE_LOCAL_STORAGE "Enable coroutine local storage" OFF)

add_library(${PROJECT_NAME} INTERFACE)
target_include_directories(${PROJECT_NAME} INTERFACE include)

if (CORO_DISABLE_EXCEPTION)
    target_compile_definitions(${PROJECT_NAME} INTERFACE CORO_DISABLE_EXCEPTION)
endif ()
if (CORO_ENABLE_LOCAL_STORAGE)
    target_compile_definitions(${PROJECT_NAME} INTERFACE CORO_ENABLE_LOCAL_STORAGE)
endif ()

# Test
option(CORO_BUILD_TEST "Build tests" OFF)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(CORO_BUILD_TEST ON)
endif ()

if (CORO_BUILD_TEST)
    option(CORO_ENABLE_SANITIZE_ADDRESS "" OFF)
    option(CORO_ENABLE_SANITIZE_THREAD "" OFF)
    option(CORO_TEST_RUNNER_VERY_SLOW "" OFF)
    # github actions runner of macOS is very slow
    if ($ENV{GITHUB_ACTIONS} AND $ENV{RUNNER_OS} STREQUAL "macOS")
        set(CORO_TEST_RUNNER_VERY_SLOW ON)
        add_compile_definitions(CORO_TEST_RUNNER_VERY_SLOW)
    endif ()

    if (CORO_ENABLE_SANITIZE_ADDRESS)
        add_compile_options(-fsanitize=address)
        add_link_options(-fsanitize=address)
    endif ()
    if (CORO_ENABLE_SANITIZE_THREAD)
        add_compile_options(-fsanitize=thread)
        add_link_options(-fsanitize=thread)
    endif ()

    if (CORO_DISABLE_EXCEPTION)
        if (MSVC)
            # MSVC: Remove default exception handling flags and add disable flags
            string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
            string(REPLACE "/EHs" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
            string(REPLACE "/EHc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
            add_compile_options(/EHs-c-)
            add_compile_definitions(_HAS_EXCEPTIONS=0)
        else ()
            # GCC/Clang
            add_compile_options(-fno-exceptions)
        endif ()
    endif ()

    include_directories(test/detail)
    link_libraries(${PROJECT_NAME})

    # test task
    set(TARGET_NAME ${PROJECT_NAME}_task)
    add_executable(${TARGET_NAME} test/coro_task.cpp)

    # test task verbose
    set(TARGET_NAME ${PROJECT_NAME}_task_verbose)
    add_executable(${TARGET_NAME} test/coro_task.cpp)
    target_compile_definitions(${TARGET_NAME} PUBLIC CORO_DEBUG_LEAK_LOG=LOG CORO_DEBUG_LIFECYCLE=LOG)

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

    # test broadcast
    set(TARGET_NAME ${PROJECT_NAME}_broadcast)
    add_executable(${TARGET_NAME} test/coro_broadcast.cpp)

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

    # test event
    set(TARGET_NAME ${PROJECT_NAME}_event)
    add_executable(${TARGET_NAME} test/coro_event.cpp)

    # test semaphore
    set(TARGET_NAME ${PROJECT_NAME}_semaphore)
    add_executable(${TARGET_NAME} test/coro_semaphore.cpp)

    # test semaphore multi-threaded
    set(TARGET_NAME ${PROJECT_NAME}_semaphore_mt)
    add_executable(${TARGET_NAME} test/coro_semaphore_mt.cpp)

    # test multi thread
    set(TARGET_NAME ${PROJECT_NAME}_multi_thread)
    add_executable(${TARGET_NAME} test/coro_multi_thread.cpp)

    # test multi thread(use single thread)
    set(TARGET_NAME ${PROJECT_NAME}_multi_thread_st)
    add_executable(${TARGET_NAME} test/coro_multi_thread.cpp)
    target_compile_definitions(${TARGET_NAME} PRIVATE CORO_USE_SINGLE_THREAD)

    # test coro_local (requires CORO_ENABLE_LOCAL_STORAGE)
    set(TARGET_NAME ${PROJECT_NAME}_coro_local)
    add_executable(${TARGET_NAME} test/coro_local.cpp)
    target_compile_definitions(${TARGET_NAME} PRIVATE CORO_ENABLE_LOCAL_STORAGE)

    message(STATUS "ASIO_PATH: $ENV{ASIO_PATH}")
    # test asio adaptor (optional, requires asio and exceptions enabled)
    set(TARGET_NAME ${PROJECT_NAME}_asio_adaptor)
    if (NOT "$ENV{ASIO_PATH}" STREQUAL "" AND NOT CORO_DISABLE_EXCEPTION)
        add_executable(${TARGET_NAME} test/coro_asio_adaptor.cpp)
        target_include_directories(${TARGET_NAME} PRIVATE $ENV{ASIO_PATH})
        target_compile_definitions(${TARGET_NAME} PRIVATE ASIO_NO_DEPRECATED)
        if (MINGW)
            target_compile_definitions(${TARGET_NAME} PRIVATE _WIN32_WINNT=0x0601)
            target_link_libraries(${TARGET_NAME} PRIVATE ws2_32 wsock32)
        endif ()
    else ()
        add_executable(${TARGET_NAME} test/coro_empty.cpp)
    endif ()
endif ()
