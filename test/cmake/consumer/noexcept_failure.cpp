#define CORO_DEBUG_PROMISE_LEAK
void test_leak_log(const char*, ...);
#define CORO_DEBUG_LEAK_LOG test_leak_log
void test_lifecycle_log(const char*, ...);
#define CORO_DEBUG_LIFECYCLE test_lifecycle_log

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "log.h"
#include "utils.hpp"

static_assert(std::is_nothrow_default_constructible_v<debug_coro_promise>);
static_assert(noexcept(std::declval<coro::awaitable_promise<int>&>().initial_suspend()));
static_assert(noexcept(std::declval<coro::awaitable_promise<void>&>().initial_suspend()));

namespace {
bool body_unwound = false;
bool expected_body_unwound = false;
bool expected_body_failure = false;
bool diagnostic_injected = false;
const char* fail_log_prefix = nullptr;
struct diagnostic_failure {};

struct body_guard {
  ~body_guard() {
    body_unwound = true;
  }
};

template <typename T>
coro::async<T> task(bool fail, [[maybe_unused]] std::shared_ptr<int> token) {
  body_guard guard;
  if (fail) {
    throw std::bad_alloc();
  }
  if constexpr (std::is_void_v<T>) {
    co_return;
  } else {
    co_return 42;
  }
}

void expect_termination() {
  auto error = std::current_exception();
  if (!error || body_unwound != expected_body_unwound) {
    std::_Exit(2);
  }
  try {
    std::rethrow_exception(error);
  } catch (const std::bad_alloc&) {
    std::_Exit(expected_body_failure ? 0 : 3);
  } catch (const diagnostic_failure&) {
    std::_Exit(!expected_body_failure && diagnostic_injected && !fail_log_prefix ? 0 : 4);
  } catch (...) {
    std::_Exit(5);
  }
}

void inject_diagnostic_failure(const char* format) {
  if (fail_log_prefix && std::strncmp(format, fail_log_prefix, std::strlen(fail_log_prefix)) == 0) {
    fail_log_prefix = nullptr;
    diagnostic_injected = true;
    throw diagnostic_failure{};
  }
}

template <typename T>
void test_normal_completion() {
  auto token = std::make_shared<int>(0);
  {
    auto value = task<T>(false, token);
    ASSERT(token.use_count() == 2);
    auto moved = std::move(value);
    ASSERT(!value.current_coro_handle_);
    auto replacement = task<T>(false, token);
    replacement = std::move(moved);
    ASSERT(token.use_count() == 2);
    ASSERT(!moved.current_coro_handle_);
    replacement.current_coro_handle_.resume();
    ASSERT(replacement.await_ready());
    if constexpr (std::is_void_v<T>) {
      replacement.await_resume();
    } else {
      ASSERT(replacement.await_resume() == 42);
    }
    ASSERT(body_unwound);
  }
  ASSERT(token.use_count() == 1);
  check_coro_leak();
}

template <typename T>
void test_failure(const char* mode) {
  auto token = std::make_shared<int>(0);
  if (std::strcmp(mode, "void") == 0 || std::strcmp(mode, "value") == 0) {
    expected_body_failure = true;
    expected_body_unwound = true;
    auto value = task<T>(true, token);
    value.current_coro_handle_.resume();
    return;
  }
  if (std::strncmp(mode, "initial_", 8) == 0) {
    fail_log_prefix = "promise: initial_suspend:";
  } else if (std::strncmp(mode, "leak_", 5) == 0) {
    fail_log_prefix = "new promise:";
  } else if (std::strncmp(mode, "log_return_", 11) == 0) {
    fail_log_prefix = "promise: get_return_object:";
  } else if (std::strncmp(mode, "log_new_", 8) == 0) {
    fail_log_prefix = "awaitable: new:";
  }
  auto value = task<T>(false, token);
  if (std::strncmp(mode, "log_move_", 9) == 0) {
    fail_log_prefix = "awaitable: move(c):";
    auto moved = std::move(value);
  } else if (std::strncmp(mode, "log_assign_", 11) == 0) {
    auto replacement = task<T>(false, token);
    fail_log_prefix = "awaitable: move(=):";
    replacement = std::move(value);
  } else if (std::strncmp(mode, "log_free_promise_", 17) == 0) {
    fail_log_prefix = "free promise:";
  } else if (std::strncmp(mode, "log_free_", 9) == 0) {
    fail_log_prefix = "awaitable: free:";
  } else if (std::strncmp(mode, "log_final_", 10) == 0) {
    fail_log_prefix = "promise: final_suspend:";
    expected_body_unwound = true;
    value.current_coro_handle_.resume();
  } else if (std::strncmp(mode, "log_ready_", 10) == 0) {
    fail_log_prefix = "awaitable: await_ready:";
    (void)value.await_ready();
  } else {
    ASSERT(diagnostic_injected);
  }
}
}  // namespace

void test_leak_log(const char* format, ...) {
  inject_diagnostic_failure(format);
}

void test_lifecycle_log(const char* format, ...) {
  inject_diagnostic_failure(format);
}

int main(int argc, char** argv) {
  ASSERT(argc == 2);
  if (std::strcmp(argv[1], "normal") == 0) {
    test_normal_completion<void>();
    body_unwound = false;
    test_normal_completion<int>();
    return 0;
  }
  std::set_terminate(expect_termination);
  if (std::strcmp(argv[1], "void") == 0 || std::strstr(argv[1], "_void")) {
    test_failure<void>(argv[1]);
  } else {
    test_failure<int>(argv[1]);
  }
  // These cases must terminate, never catch or silently discard a diagnostic error.
  return 1;
}
