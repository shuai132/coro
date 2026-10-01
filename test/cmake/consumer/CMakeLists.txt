cmake_minimum_required(VERSION 3.15)
project(coro_consumer C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# A caller can have an unrelated feature check in progress while adding the library.
set(CMAKE_REQUIRED_LIBRARIES coro_missing_probe_only_library)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../.." coro)

if (TARGET coro_task)
    message(FATAL_ERROR "Subproject tests should be disabled by default")
endif ()

add_executable(consumer main.cpp helper.c)
if (NOT MSVC)
    target_compile_options(consumer PRIVATE "$<$<COMPILE_LANGUAGE:C>:-Werror>")
endif ()
target_link_libraries(consumer PRIVATE coro)
target_include_directories(consumer PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../detail")

add_library(log_header_consumer OBJECT log.cpp)
target_include_directories(log_header_consumer PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../detail")

enable_testing()
add_test(NAME consumer_run COMMAND consumer)

# The header-only target must carry native thread link requirements for its executors.
add_executable(threaded_consumer threaded.cpp)
target_link_libraries(threaded_consumer PRIVATE coro)
add_test(NAME threaded_consumer_run COMMAND threaded_consumer)

# Compile the portable component headers independently of the host test helpers.
add_library(threadless_consumer OBJECT ../threadless.cpp)
target_link_libraries(threadless_consumer PRIVATE coro)

# The public macro can disable propagation even when the consumer still enables C++ exceptions.
add_executable(noexcept_failure_consumer noexcept_failure.cpp)
target_link_libraries(noexcept_failure_consumer PRIVATE coro)
target_include_directories(noexcept_failure_consumer PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../detail")
target_compile_definitions(noexcept_failure_consumer PRIVATE CORO_DISABLE_EXCEPTION)
if (MSVC)
    target_compile_options(noexcept_failure_consumer PRIVATE /EHsc)
else ()
    target_compile_options(noexcept_failure_consumer PRIVATE -fexceptions)
endif ()
foreach (mode normal void value initial_void initial_value leak_void leak_value)
    add_test(NAME noexcept_failure_${mode} COMMAND noexcept_failure_consumer ${mode})
endforeach ()
