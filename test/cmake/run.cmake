cmake_minimum_required(VERSION 3.15)

get_filename_component(CORO_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CORO_REGRESSION_BINARY_DIR "${CORO_SOURCE_DIR}/build/cmake-regression")

function(configure_project name source)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${CORO_CONFIGURE_ENV}
            "${CMAKE_COMMAND}" -S "${source}" -B "${CORO_REGRESSION_BINARY_DIR}/${name}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if (NOT result EQUAL 0)
        message(FATAL_ERROR "Configuration ${name} failed:\n${output}\n${error}")
    endif ()
endfunction()

function(test_slow_runner name enabled expected)
    set(api_dir "${CORO_REGRESSION_BINARY_DIR}/${name}/.cmake/api/v1")
    file(MAKE_DIRECTORY "${api_dir}/query")
    file(WRITE "${api_dir}/query/codemodel-v2" "")
    file(REMOVE_RECURSE "${api_dir}/reply")
    set(CORO_CONFIGURE_ENV "GITHUB_ACTIONS=" "RUNNER_OS=" ${ARGN})
    configure_project("${name}" "${CORO_SOURCE_DIR}" -DCORO_BUILD_TEST=ON -DCORO_TEST_RUNNER_VERY_SLOW=${enabled})
    file(GLOB targets "${api_dir}/reply/target-coro_when-*.json")
    if (NOT targets)
        message(FATAL_ERROR "Configuration ${name}: missing coro_when compiler model")
    endif ()
    foreach (target IN LISTS targets)
        file(READ "${target}" model)
        if (model MATCHES "\"define\"[ \t\r\n]*:[ \t\r\n]*\"CORO_TEST_RUNNER_VERY_SLOW\"")
            set(actual ON)
        else ()
            set(actual OFF)
        endif ()
        if (NOT actual STREQUAL expected)
            message(FATAL_ERROR "Configuration ${name}: expected slow-runner definition ${expected}, found ${actual}")
        endif ()
    endforeach ()
endfunction()

function(test_asio_configuration label expected_path disable_exception)
    set(api_dir "${CORO_REGRESSION_BINARY_DIR}/asio-path/.cmake/api/v1")
    file(MAKE_DIRECTORY "${api_dir}/query")
    file(WRITE "${api_dir}/query/codemodel-v2" "")
    file(REMOVE_RECURSE "${api_dir}/reply")
    set(CORO_CONFIGURE_ENV ${ARGN})
    configure_project(asio-path "${CORO_SOURCE_DIR}" -DCORO_BUILD_TEST=ON -DCORO_DISABLE_EXCEPTION=${disable_exception})
    file(GLOB targets "${api_dir}/reply/target-coro_asio_adaptor-*.json")
    if (NOT targets)
        message(FATAL_ERROR "Configuration ${label}: missing ASIO test compiler model")
    endif ()
    if (expected_path STREQUAL "" OR disable_exception)
        set(expected_source "test/coro_empty.cpp")
    else ()
        set(expected_source "test/coro_asio_adaptor.cpp")
    endif ()
    foreach (target IN LISTS targets)
        file(READ "${target}" model)
        string(FIND "${model}" "${expected_source}" source_index)
        if (source_index EQUAL -1)
            message(FATAL_ERROR "Configuration ${label}: expected ${expected_source}")
        endif ()
        if (NOT expected_path STREQUAL "" AND NOT disable_exception)
            string(FIND "${model}" "${expected_path}" include_index)
            if (include_index EQUAL -1)
                message(FATAL_ERROR "Configuration ${label}: ASIO include directory was not retained")
            endif ()
        endif ()
    endforeach ()
endfunction()

function(expect_test_targets name expected)
    file(READ "${CORO_REGRESSION_BINARY_DIR}/${name}/CMakeFiles/TargetDirectories.txt" targets)
    if (targets MATCHES "coro_task\\.dir")
        set(actual ON)
    else ()
        set(actual OFF)
    endif ()
    if (NOT actual STREQUAL expected)
        message(FATAL_ERROR "Configuration ${name}: expected test targets ${expected}, found ${actual}")
    endif ()
endfunction()

function(test_exception_configuration name compiler_exceptions expected build_consumer)
    configure_project("${name}" "${CMAKE_CURRENT_LIST_DIR}/exceptions"
        -UCORO_DISABLE_EXCEPTION -UPARENT_DISABLE_EXCEPTION
        -DCOMPILER_EXCEPTIONS=${compiler_exceptions} -DEXPECT_DISABLED=${expected}
        -DCORO_BUILD_TEST=OFF ${ARGN})
    if (build_consumer)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" --build "${CORO_REGRESSION_BINARY_DIR}/${name}" --config Release
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
        )
        if (NOT result EQUAL 0)
            message(FATAL_ERROR "Configuration ${name}: consumer build failed:\n${output}\n${error}")
        endif ()
        execute_process(
            COMMAND "${CMAKE_CTEST_COMMAND}" -C Release --output-on-failure
            WORKING_DIRECTORY "${CORO_REGRESSION_BINARY_DIR}/${name}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
        )
        if (NOT result EQUAL 0)
            message(FATAL_ERROR "Configuration ${name}: consumer run failed:\n${output}\n${error}")
        endif ()
    endif ()
endfunction()

test_exception_configuration(exceptions-auto-disabled OFF ON ON)
test_exception_configuration(exceptions-auto-enabled ON OFF ON)
# Cached choices survive a compiler-mode change; clearing the option must recheck both directions.
configure_project(exceptions-auto-enabled "${CMAKE_CURRENT_LIST_DIR}/exceptions"
    -DCOMPILER_EXCEPTIONS=OFF -DEXPECT_DISABLED=OFF -DCORO_BUILD_TEST=OFF)
test_exception_configuration(exceptions-auto-enabled OFF ON ON)
test_exception_configuration(exceptions-auto-enabled ON OFF ON)
test_exception_configuration(exceptions-explicit-on ON ON ON -DCORO_DISABLE_EXCEPTION=ON)
test_exception_configuration(exceptions-explicit-off ON OFF ON -DCORO_DISABLE_EXCEPTION=OFF)
test_exception_configuration(exceptions-explicit-on-disabled OFF ON ON -DCORO_DISABLE_EXCEPTION=ON)
# An explicit OFF is preserved even if it contradicts the compiler flags; only configure this case.
test_exception_configuration(exceptions-explicit-off-disabled OFF OFF OFF -DCORO_DISABLE_EXCEPTION=OFF)
test_exception_configuration(exceptions-parent-on ON ON ON -DPARENT_DISABLE_EXCEPTION=ON)
test_exception_configuration(exceptions-parent-off-disabled OFF OFF OFF -DPARENT_DISABLE_EXCEPTION=OFF)

configure_project(tests-default "${CORO_SOURCE_DIR}")
expect_test_targets(tests-default ON)
configure_project(tests-off "${CORO_SOURCE_DIR}" -DCORO_BUILD_TEST=OFF)
expect_test_targets(tests-off OFF)
configure_project(tests-on "${CORO_SOURCE_DIR}" -DCORO_BUILD_TEST=ON)
expect_test_targets(tests-on ON)

test_slow_runner(slow-runner-on ON ON)
test_slow_runner(slow-runner-off OFF OFF)
test_slow_runner(slow-runner-auto OFF ON GITHUB_ACTIONS=true RUNNER_OS=macOS)

# Inspect target selection and include paths without requiring ASIO headers to be installed.
set(asio_path_one "${CORO_REGRESSION_BINARY_DIR}/asio headers one")
set(asio_path_two "${CORO_REGRESSION_BINARY_DIR}/asio-headers-two")
file(MAKE_DIRECTORY "${asio_path_one}" "${asio_path_two}")
test_asio_configuration(asio-initial "${asio_path_one}" OFF "ASIO_PATH=${asio_path_one}")
test_asio_configuration(asio-without-env "${asio_path_one}" OFF --unset=ASIO_PATH)
test_asio_configuration(asio-noexcept "${asio_path_one}" ON --unset=ASIO_PATH)
test_asio_configuration(asio-exceptions-restored "${asio_path_one}" OFF --unset=ASIO_PATH)
test_asio_configuration(asio-path-updated "${asio_path_two}" OFF "ASIO_PATH=${asio_path_two}")
test_asio_configuration(asio-updated-without-env "${asio_path_two}" OFF --unset=ASIO_PATH)
test_asio_configuration(asio-cleared "" OFF ASIO_PATH=)
test_asio_configuration(asio-still-cleared "" OFF --unset=ASIO_PATH)

configure_project(consumer "${CMAKE_CURRENT_LIST_DIR}/consumer")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${CORO_REGRESSION_BINARY_DIR}/consumer" --config Release
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer build failed:\n${output}\n${error}")
endif ()
execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" -C Release --output-on-failure
    WORKING_DIRECTORY "${CORO_REGRESSION_BINARY_DIR}/consumer"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer test failed:\n${output}\n${error}")
endif ()
message(STATUS "CMake integration regressions passed")
