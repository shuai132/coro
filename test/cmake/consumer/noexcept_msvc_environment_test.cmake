cmake_minimum_required(VERSION 3.15)

# A plain Visual Studio CTest process has no developer environment. Use a mock setup
# script and compiler to verify that preprocessing runs in the environment it creates.
set(directory "${BINARY_DIR}/mock compiler environment")
file(MAKE_DIRECTORY "${directory}")
set(setup "${directory}/setup.cmd")
set(compiler "${directory}/compiler.cmd")
file(WRITE "${setup}" "@echo off\r\nset CORO_TEST_COMPILER_ENV=ready\r\nexit /b 0\r\n")
file(WRITE "${compiler}" [=[@echo off
if not "%CORO_TEST_COMPILER_ENV%"=="ready" exit /b 7
echo #line 1 "fixture/include/coro/coro.hpp"
echo void run^(^) { try {} catch ^(...^) {} }
]=])
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${compiler}" -DCOMPILER_ID=MSVC
        "-DMSVC_ENV_SCRIPT=${setup}" -DMSVC_TARGET_ARCH=x64
        "-DINCLUDE_DIR=${directory}" "-DSOURCE=${directory}/source.cpp"
        "-DBINARY_DIR=${directory}" -DDISABLE_EXCEPTION=OFF
        -P "${CMAKE_CURRENT_LIST_DIR}/noexcept_source.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "MSVC preprocessing environment regression failed:\n${output}\n${error}")
endif ()
message(STATUS "MSVC preprocessing environment regression passed")
