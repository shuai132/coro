cmake_minimum_required(VERSION 3.15)
project(coro_consumer C CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../warnings.cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# A caller can have an unrelated feature check in progress while adding the library.
set(CMAKE_REQUIRED_LIBRARIES coro_missing_probe_only_library)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../.." coro)
get_target_property(coro_interface_options coro INTERFACE_COMPILE_OPTIONS)
if (coro_interface_options MATCHES "-Werror|/WX")
    message(FATAL_ERROR "The library must not export warnings-as-errors to consumers")
endif ()

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
add_test(NAME noexcept_source_scanner
    COMMAND "${CMAKE_COMMAND}" "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_LIST_DIR}/noexcept_source_scan_test.cmake"
)
if (WIN32)
    add_test(NAME noexcept_msvc_environment
        COMMAND "${CMAKE_COMMAND}" "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_LIST_DIR}/noexcept_msvc_environment_test.cmake"
    )
endif ()

# Preprocess with compiler exceptions enabled to distinguish the public macro from -fno-exceptions.
set(CORO_SOURCE_ASIO_INCLUDE_DIR "")
if (DEFINED ENV{ASIO_PATH})
    get_filename_component(asio_include "$ENV{ASIO_PATH}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")
    if (EXISTS "${asio_include}/asio.hpp")
        set(CORO_SOURCE_ASIO_INCLUDE_DIR "${asio_include}")
    endif ()
endif ()
add_library(noexcept_source_consumer OBJECT noexcept_source.cpp)
target_link_libraries(noexcept_source_consumer PRIVATE coro)
target_compile_definitions(noexcept_source_consumer PRIVATE CORO_DISABLE_EXCEPTION)
if (MSVC)
    target_compile_options(noexcept_source_consumer PRIVATE /EHsc)
else ()
    target_compile_options(noexcept_source_consumer PRIVATE -fexceptions)
endif ()
if (NOT "${CORO_SOURCE_ASIO_INCLUDE_DIR}" STREQUAL "")
    target_include_directories(noexcept_source_consumer SYSTEM PRIVATE "${CORO_SOURCE_ASIO_INCLUDE_DIR}")
    target_compile_definitions(noexcept_source_consumer PRIVATE CORO_TEST_WITH_ASIO NOMINMAX)
    if (WIN32)
        target_compile_definitions(noexcept_source_consumer PRIVATE _WIN32_WINNT=0x0601)
    endif ()
endif ()
set(CORO_SOURCE_MSVC_ENV_SCRIPT "")
set(CORO_SOURCE_MSVC_TARGET_ARCH "${CMAKE_VS_PLATFORM_NAME}")
if (MSVC AND CMAKE_GENERATOR MATCHES "^Visual Studio")
    # MSBuild initializes SDK paths for builds, but a direct cl.exe invocation in CTest does not.
    string(REGEX REPLACE ",.*$" "" vs_instance "${CMAKE_GENERATOR_INSTANCE}")
    set(CORO_SOURCE_MSVC_ENV_SCRIPT "${vs_instance}/Common7/Tools/VsDevCmd.bat")
    if (NOT EXISTS "${CORO_SOURCE_MSVC_ENV_SCRIPT}")
        message(FATAL_ERROR "Cannot locate the selected Visual Studio developer environment script")
    endif ()
    if (CORO_SOURCE_MSVC_TARGET_ARCH STREQUAL "Win32")
        set(CORO_SOURCE_MSVC_TARGET_ARCH x86)
    endif ()
endif ()
foreach (disabled ON OFF)
    add_test(NAME noexcept_source_${disabled}
        COMMAND "${CMAKE_COMMAND}"
            "-DCOMPILER=${CMAKE_CXX_COMPILER}"
            "-DCOMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
            "-DCOMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}"
            "-DMSVC_ENV_SCRIPT=${CORO_SOURCE_MSVC_ENV_SCRIPT}"
            "-DMSVC_TARGET_ARCH=${CORO_SOURCE_MSVC_TARGET_ARCH}"
            "-DINCLUDE_DIR=${CMAKE_CURRENT_LIST_DIR}/../../../include"
            "-DSOURCE=${CMAKE_CURRENT_LIST_DIR}/noexcept_source.cpp"
            "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DASIO_INCLUDE_DIR=${CORO_SOURCE_ASIO_INCLUDE_DIR}"
            "-DDISABLE_EXCEPTION=${disabled}"
            -P "${CMAKE_CURRENT_LIST_DIR}/noexcept_source.cmake"
    )
endforeach ()

# The header-only target must carry native thread link requirements for its executors.
add_executable(threaded_consumer threaded.cpp)
target_link_libraries(threaded_consumer PRIVATE coro)
add_test(NAME threaded_consumer_run COMMAND threaded_consumer)

# Compile the portable component headers independently of the host test helpers.
add_library(threadless_consumer OBJECT ../threadless.cpp)
target_link_libraries(threadless_consumer PRIVATE coro)

# The public macro removes library exception handling even when compiler exceptions remain enabled.
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

foreach (phase return new move assign free free_promise final ready)
    foreach (result void value)
        add_test(NAME noexcept_log_${phase}_${result} COMMAND noexcept_failure_consumer log_${phase}_${result})
    endforeach ()
endforeach ()
