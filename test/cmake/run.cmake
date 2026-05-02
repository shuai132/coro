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

configure_project(tests-default "${CORO_SOURCE_DIR}")
expect_test_targets(tests-default ON)
configure_project(tests-off "${CORO_SOURCE_DIR}" -DCORO_BUILD_TEST=OFF)
expect_test_targets(tests-off OFF)
configure_project(tests-on "${CORO_SOURCE_DIR}" -DCORO_BUILD_TEST=ON)
expect_test_targets(tests-on ON)

test_slow_runner(slow-runner-on ON ON)
test_slow_runner(slow-runner-off OFF OFF)
test_slow_runner(slow-runner-auto OFF ON GITHUB_ACTIONS=true RUNNER_OS=macOS)

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
