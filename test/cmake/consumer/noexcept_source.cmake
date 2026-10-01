cmake_minimum_required(VERSION 3.15)

if (COMPILER_ID STREQUAL "MSVC")
    set(flags /nologo /std:c++20 /Zc:preprocessor /EHsc /E "/I${INCLUDE_DIR}")
    set(define_prefix /D)
else ()
    set(flags -std=c++20 -fexceptions -E "-I${INCLUDE_DIR}")
    set(define_prefix -D)
    if (COMPILER_ID STREQUAL "GNU" AND COMPILER_VERSION VERSION_LESS 11)
        list(APPEND flags -fcoroutines)
    endif ()
endif ()
list(APPEND flags "${define_prefix}NOMINMAX")
if (WIN32)
    list(APPEND flags "${define_prefix}_WIN32_WINNT=0x0601")
endif ()
if (DISABLE_EXCEPTION)
    list(APPEND flags "${define_prefix}CORO_DISABLE_EXCEPTION")
endif ()
if (NOT "${ASIO_INCLUDE_DIR}" STREQUAL "")
    list(APPEND flags "${define_prefix}CORO_TEST_WITH_ASIO")
    if (COMPILER_ID STREQUAL "MSVC")
        list(APPEND flags "/I${ASIO_INCLUDE_DIR}")
    else ()
        list(APPEND flags "-I${ASIO_INCLUDE_DIR}")
    endif ()
endif ()

set(output "${BINARY_DIR}/noexcept-source-${DISABLE_EXCEPTION}.ii")
execute_process(
    COMMAND "${COMPILER}" ${flags} "${SOURCE}"
    OUTPUT_FILE "${output}"
    ERROR_VARIABLE error
    RESULT_VARIABLE result
)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "Exception-source preprocessing failed:\n${error}")
endif ()

set(INPUT "${output}")
include("${CMAKE_CURRENT_LIST_DIR}/noexcept_source_scan.cmake")
