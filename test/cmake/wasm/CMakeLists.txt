cmake_minimum_required(VERSION 3.15)
project(coro_wasm_consumer CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../warnings.cmake")

enable_testing()
set(CORO_BUILD_TEST OFF CACHE BOOL "Build the native test suite")
set(CORO_BUILD_EXAMPLES ON CACHE BOOL "Build standalone usage examples")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../.." coro)

# Reuse a runtime regression with assertions, exception paths and promise leak checks.
add_executable(coro_wasm_core_move_only ../../coro_core_move_only.cpp)
target_include_directories(coro_wasm_core_move_only PRIVATE ../../detail)
target_link_libraries(coro_wasm_core_move_only PRIVATE coro)
if (EMSCRIPTEN)
    # The test logger's native thread-ID decoration assumes a Linux or macOS API.
    target_compile_definitions(coro_wasm_core_move_only PRIVATE L_O_G_DISABLE_THREAD_ID)
endif ()
add_test(NAME coro_wasm_core_move_only COMMAND coro_wasm_core_move_only)
set_tests_properties(coro_wasm_core_move_only PROPERTIES TIMEOUT 30)
