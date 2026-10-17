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

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

function(_coro_detect_exception_support result)
    cmake_push_check_state(RESET)
    # Compile only, so embedded toolchains do not need a runnable/linkable probe executable.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    # Recheck the current flags if the user clears the public option to restore automatic selection.
    unset(CORO_COMPILER_HAS_EXCEPTIONS CACHE)
    check_cxx_source_compiles("#if !defined(__cpp_exceptions) && !defined(_CPPUNWIND)
#error Compiler exceptions are disabled
#endif
int coro_exception_probe() { return 0; }" CORO_COMPILER_HAS_EXCEPTIONS)
    cmake_pop_check_state()
    set(${result} "${CORO_COMPILER_HAS_EXCEPTIONS}" PARENT_SCOPE)
endfunction()

set(CORO_DISABLE_EXCEPTION_DEFAULT OFF)
if (NOT DEFINED CORO_DISABLE_EXCEPTION)
    _coro_detect_exception_support(CORO_COMPILER_EXCEPTIONS_ENABLED)
    if (NOT CORO_COMPILER_EXCEPTIONS_ENABLED)
        set(CORO_DISABLE_EXCEPTION_DEFAULT ON)
    endif ()
    unset(CORO_COMPILER_EXCEPTIONS_ENABLED)
endif ()
option(CORO_DISABLE_EXCEPTION "Disable exception support" ${CORO_DISABLE_EXCEPTION_DEFAULT})
unset(CORO_DISABLE_EXCEPTION_DEFAULT)
option(CORO_ENABLE_LOCAL_STORAGE "Enable coroutine local storage" OFF)
option(CORO_DEBUG_ASYNC_STACK "Enable asynchronous debugger metadata" OFF)

add_library(${PROJECT_NAME} INTERFACE)
target_include_directories(${PROJECT_NAME} INTERFACE include)
target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_20)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Symmetric coroutine transfers otherwise accumulate native stack in GCC Debug builds.
    target_compile_options(${PROJECT_NAME} INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:-foptimize-sibling-calls>")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
        # GCC 10 does not enable coroutines with -std=c++20 alone.
        target_compile_options(${PROJECT_NAME} INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:-fcoroutines>")
    endif ()
endif ()

# Native executors and multithreaded primitives need the platform's thread link flags.
# A threadless standard library can still consume the explicit _st component headers.
cmake_push_check_state(RESET)
check_cxx_source_compiles("#include <mutex>
int main() { return sizeof(std::mutex) == 0; }" CORO_HAS_STD_MUTEX)
if (CORO_HAS_STD_MUTEX)
    # Compile-only cross-toolchain probes cannot prove that a thread library is linkable.
    if (NOT TARGET Threads::Threads AND NOT CMAKE_TRY_COMPILE_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
        set(THREADS_PREFER_PTHREAD_FLAG TRUE)
        find_package(Threads QUIET)
    endif ()
    if (TARGET Threads::Threads)
        target_link_libraries(${PROJECT_NAME} INTERFACE Threads::Threads)
    endif ()
endif ()
cmake_pop_check_state()

if (CORO_DEBUG_ASYNC_STACK)
    target_compile_definitions(${PROJECT_NAME} INTERFACE CORO_DEBUG_ASYNC_STACK)
endif ()

if (CORO_DISABLE_EXCEPTION)
    target_compile_definitions(${PROJECT_NAME} INTERFACE CORO_DISABLE_EXCEPTION)
endif ()
if (CORO_ENABLE_LOCAL_STORAGE)
    target_compile_definitions(${PROJECT_NAME} INTERFACE CORO_ENABLE_LOCAL_STORAGE)
endif ()

# Test
set(CORO_BUILD_TEST_DEFAULT OFF)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(CORO_BUILD_TEST_DEFAULT ON)
endif ()
option(CORO_BUILD_TEST "Build tests" ${CORO_BUILD_TEST_DEFAULT})
unset(CORO_BUILD_TEST_DEFAULT)

# Examples
set(CORO_BUILD_EXAMPLES_DEFAULT OFF)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(CORO_BUILD_EXAMPLES_DEFAULT ON)
endif ()
option(CORO_BUILD_EXAMPLES "Build standalone usage examples" ${CORO_BUILD_EXAMPLES_DEFAULT})
unset(CORO_BUILD_EXAMPLES_DEFAULT)

if (CORO_BUILD_TEST OR CORO_BUILD_EXAMPLES)
    option(CORO_WARNINGS_AS_ERRORS "Treat project build warnings as errors" ON)
    if (CORO_WARNINGS_AS_ERRORS)
        if (MSVC)
            add_compile_options(/WX)
        else ()
            add_compile_options(-Werror)
        endif ()
    endif ()
    option(CORO_ENABLE_SANITIZE_ADDRESS "" OFF)
    option(CORO_ENABLE_SANITIZE_THREAD "" OFF)
    option(CORO_TEST_RUNNER_VERY_SLOW "Relax test timing for slow runners" OFF)
    # github actions runner of macOS is very slow
    if ($ENV{GITHUB_ACTIONS} AND $ENV{RUNNER_OS} STREQUAL "macOS")
        set(CORO_TEST_RUNNER_VERY_SLOW ON)
    endif ()
    if (CORO_TEST_RUNNER_VERY_SLOW)
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
endif ()

if (CORO_BUILD_TEST)
    include_directories(test/detail)
    link_libraries(${PROJECT_NAME})

    # test task
    set(TARGET_NAME ${PROJECT_NAME}_task)
    add_executable(${TARGET_NAME} test/coro_task.cpp)

    # test task verbose
    set(TARGET_NAME ${PROJECT_NAME}_task_verbose)
    add_executable(${TARGET_NAME} test/coro_task.cpp)
    target_compile_definitions(${TARGET_NAME} PUBLIC CORO_DEBUG_LEAK_LOG=LOG CORO_DEBUG_LIFECYCLE=LOG)

    # test core lifetime
    set(TARGET_NAME ${PROJECT_NAME}_core_lifetime)
    add_executable(${TARGET_NAME} test/coro_core_lifetime_static.cpp test/coro_core_lifetime.cpp)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-fcoro-aligned-allocation" CORO_TEST_HAS_ALIGNED_COROUTINE_ALLOCATION)
    if (CORO_TEST_HAS_ALIGNED_COROUTINE_ALLOCATION)
        target_compile_options(${TARGET_NAME} PRIVATE -fcoro-aligned-allocation)
        target_compile_definitions(${TARGET_NAME} PRIVATE CORO_TEST_ALIGNED_COROUTINE_ALLOCATION)
    endif ()

    # test asynchronous debugger metadata
    set(TARGET_NAME ${PROJECT_NAME}_debug)
    add_executable(${TARGET_NAME} test/coro_debug_static.cpp test/coro_debug.cpp)
    target_compile_definitions(${TARGET_NAME} PRIVATE CORO_DEBUG_ASYNC_STACK)

    # test core callback
    set(TARGET_NAME ${PROJECT_NAME}_core_callback)
    add_executable(${TARGET_NAME} test/coro_core_callback.cpp)
    # Strict mode exposes differences in extended integer traits on some standard libraries.
    set_target_properties(${TARGET_NAME} PROPERTIES CXX_EXTENSIONS OFF)

    # test core move-only result
    set(TARGET_NAME ${PROJECT_NAME}_core_move_only)
    add_executable(${TARGET_NAME} test/coro_core_move_only.cpp)

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

    # Reconfiguration can run without the environment used during the initial configure.
    if (DEFINED ENV{ASIO_PATH})
        set(CORO_TEST_ASIO_PATH "$ENV{ASIO_PATH}" CACHE INTERNAL "ASIO include directory for tests" FORCE)
    endif ()
    message(STATUS "ASIO_PATH: ${CORO_TEST_ASIO_PATH}")
    # test asio adaptor (optional, requires asio and exceptions enabled)
    set(TARGET_NAME ${PROJECT_NAME}_asio_adaptor)
    if (NOT "${CORO_TEST_ASIO_PATH}" STREQUAL "" AND NOT CORO_DISABLE_EXCEPTION)
        add_executable(${TARGET_NAME} test/coro_asio_adaptor.cpp)
        target_include_directories(${TARGET_NAME} SYSTEM PRIVATE "${CORO_TEST_ASIO_PATH}")
        target_compile_definitions(${TARGET_NAME} PRIVATE ASIO_NO_DEPRECATED)
        if (MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
            CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15 AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
            # GCC 15 MinGW diagnoses ASIO's inlined coroutine-frame cleanup as use-after-free.
            # Keep the diagnostic enabled and limit the inlining workaround to this test target.
            target_compile_options(${TARGET_NAME} PRIVATE -fno-inline)
        endif ()
        if (CORO_ENABLE_SANITIZE_THREAD AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            # GCC TSan cannot instrument ASIO's atomic_thread_fence calls. Keep the fences
            # and sanitizer coverage; only this third-party limitation is silenced for its test.
            target_compile_options(${TARGET_NAME} PRIVATE -Wno-tsan)
        endif ()
        if (MINGW)
            target_compile_definitions(${TARGET_NAME} PRIVATE _WIN32_WINNT=0x0601)
            target_link_libraries(${TARGET_NAME} PRIVATE ws2_32 wsock32)
        endif ()
    else ()
        add_executable(${TARGET_NAME} test/coro_empty.cpp)
    endif ()
endif ()

if (CORO_BUILD_EXAMPLES)
    enable_testing()
    foreach (example task mutex channel broadcast semaphore condition_variable wait_group latch event when timer callback coro_local)
        set(TARGET_NAME ${PROJECT_NAME}_example_${example})
        add_executable(${TARGET_NAME} examples/${example}.cpp)
        target_link_libraries(${TARGET_NAME} PRIVATE ${PROJECT_NAME})
        add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
        set_tests_properties(${TARGET_NAME} PROPERTIES TIMEOUT 15 LABELS examples)
    endforeach ()
endif ()

# Optional comparisons; dependencies are fetched only when explicitly enabled.
option(CORO_BUILD_BENCHMARKS "Build comparisons with async_simple and standalone Asio" OFF)
if (CORO_BUILD_BENCHMARKS)
    enable_testing()
    add_subdirectory(benchmark)
endif ()
