cmake_minimum_required(VERSION 3.15)

function(check_scan name disabled expected_result source)
    set(input "${BINARY_DIR}/${name}.ii")
    file(WRITE "${input}" "${source}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${input}" "-DDISABLE_EXCEPTION=${disabled}"
            -P "${CMAKE_CURRENT_LIST_DIR}/noexcept_source_scan.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    )
    if (expected_result STREQUAL "PASS")
        if (NOT result EQUAL 0)
            message(FATAL_ERROR "Source scanner ${name} failed:\n${output}\n${error}")
        endif ()
    elseif (result EQUAL 0 OR NOT error MATCHES "retained exception handling")
        message(FATAL_ERROR "Source scanner ${name} missed library exception handling:\n${output}\n${error}")
    endif ()
endfunction()

# Standard headers can contain unmatched brackets in string literals (e.g. regex tables).
# They must not change the boundaries between subsequent preprocessor lines.
set(prefix [=[# 1 "fixture/standard.hpp"
const char* bracket = "[";
]=])
set(enabled [=[# 1 "fixture/include/coro/coro.hpp"
void run() { try {} catch (...) {} }
]=])
set(disabled [=[# 1 "fixture/include/coro/coro.hpp"
void run() {}
]=])
check_scan(bracket-control OFF PASS "${prefix}${enabled}")
check_scan(bracket-detect ON FAIL "${prefix}${enabled}")
check_scan(bracket-disabled ON PASS "${prefix}${disabled}")
set(closing_prefix [=[# 1 "fixture/standard.hpp"
const char* bracket = "]";
]=])
check_scan(closing-bracket-control OFF PASS "${closing_prefix}${enabled}")
check_scan(closing-bracket-detect ON FAIL "${closing_prefix}${enabled}")

# MSVC line directives use a different prefix and can contain Windows path separators.
set(windows [=[#line 1 "fixture\include\coro\coro.hpp"
void run() { try {} catch (...) {} }
]=])
check_scan(windows-control OFF PASS "${prefix}${windows}")
check_scan(windows-detect ON FAIL "${prefix}${windows}")

# Exception keywords inside diagnostic strings and third-party code are not library handlers.
check_scan(external-handlers ON PASS [=[# 1 "fixture/asio.hpp"
void external() { try {} catch (...) {} }
# 1 "fixture/include/coro/coro.hpp"
const char* message = "try catch throw std::exception_ptr";
void run() {}
]=])
message(STATUS "Exception-source scanner regressions passed")
