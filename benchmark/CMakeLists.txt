cmake_minimum_required(VERSION 3.18)

if (CORO_DISABLE_EXCEPTION)
    message(FATAL_ERROR "The comparison requires exceptions (async_simple and Asio). Use a separate benchmark build.")
endif ()
if (CORO_DEBUG_ASYNC_STACK OR CORO_ENABLE_LOCAL_STORAGE)
    message(FATAL_ERROR "Use default coroutine settings for comparable benchmark results.")
endif ()
if (NOT CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "Configure benchmarks with -DCMAKE_BUILD_TYPE=Release")
endif ()

# Fetch headers only; do not build upstream tests or introduce core dependencies.
include(FetchContent)
FetchContent_Declare(coro_bench_async_simple
    GIT_REPOSITORY https://github.com/alibaba/async_simple.git
    GIT_TAG 0c7a12b5882b2c23628117966d85fa91f26abd8c
    SOURCE_SUBDIR coro-benchmark-headers-only)
FetchContent_Declare(coro_bench_asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG 231cb29bab30f82712fcd54faaea42424cc6e710
    SOURCE_SUBDIR coro-benchmark-headers-only)
# The nonexistent subdirectory keeps upstream build systems out of this project.
FetchContent_MakeAvailable(coro_bench_async_simple coro_bench_asio)
find_package(Threads REQUIRED)
set(benchmark_targets coro_benchmark)
if (UNIX)
    list(APPEND benchmark_targets coro_benchmark_allocations)
endif ()
foreach (target ${benchmark_targets})
    add_executable(${target} compare.cpp)
    target_link_libraries(${target} PRIVATE coro Threads::Threads)
    target_include_directories(${target} SYSTEM PRIVATE
        "${coro_bench_async_simple_SOURCE_DIR}" "${coro_bench_asio_SOURCE_DIR}/asio/include")
    target_compile_definitions(${target} PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
    if (UNIX)
        target_compile_definitions(${target} PRIVATE CORO_BENCH_POSIX_RESOURCES)
    endif ()
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else ()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif ()
    if (WIN32)
        target_link_libraries(${target} PRIVATE ws2_32 mswsock)
    endif ()
endforeach ()
add_test(NAME coro_benchmark_smoke COMMAND coro_benchmark 17 2)
add_test(NAME coro_benchmark_diagnostics COMMAND coro_benchmark --diagnostics 17 2)
set_tests_properties(coro_benchmark_smoke coro_benchmark_diagnostics PROPERTIES TIMEOUT 30)

if (UNIX)
    target_compile_definitions(coro_benchmark_allocations PRIVATE CORO_BENCH_ALLOCATIONS)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if (Python3_Interpreter_FOUND)
        add_test(NAME coro_benchmark_resources
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/resources.py"
                $<TARGET_FILE:coro_benchmark> $<TARGET_FILE:coro_benchmark_allocations> 17)
        set_tests_properties(coro_benchmark_resources PROPERTIES TIMEOUT 60)
    endif ()
endif ()
