#define CORO_DEBUG_PROMISE_LEAK
void test_lifecycle_log(const char*, ...);
#define CORO_DEBUG_LIFECYCLE test_lifecycle_log

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <type_traits>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "log.h"
#include "utils.hpp"

namespace {
bool body_unwound = false;
bool fail_initial_log = false;
struct creation_failure {};

struct body_guard {
  ~body_guard() {
    body_unwound = true;
  }
};

template <typename T>
coro::async<T> task(bool fail) {
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

template <typename T>
coro::async<T> initial_task([[maybe_unused]] std::shared_ptr<int> frame_token) {
  body_guard guard;
  if constexpr (std::is_void_v<T>) {
    co_return;
  } else {
    co_return 42;
  }
}

template <typename T>
void test_initial_failure() {
  auto frame_token = std::make_shared<int>(0);
  fail_initial_log = true;
  bool caught = false;
  try {
    auto value = initial_task<T>(frame_token);
  } catch (const creation_failure&) {
    caught = true;
  }
  ASSERT(caught);
  ASSERT(!fail_initial_log);
  ASSERT(!body_unwound);
  ASSERT(frame_token.use_count() == 1);
  check_coro_leak();
  {
    auto value = initial_task<T>(frame_token);
    value.current_coro_handle_.resume();
    if constexpr (std::is_void_v<T>) {
      value.await_resume();
    } else {
      ASSERT(value.await_resume() == 42);
    }
  }
  ASSERT(body_unwound);
  ASSERT(frame_token.use_count() == 1);
  check_coro_leak();
}

void expect_termination() {
  // Only the intended body failure should reach this handler, after local cleanup.
  auto error = std::current_exception();
  if (!error || !body_unwound) {
    std::_Exit(2);
  }
  try {
    std::rethrow_exception(error);
  } catch (const std::bad_alloc&) {
    // Termination leaves live coroutine frames; normal completion checks their release below.
    std::_Exit(0);
  } catch (...) {
    std::_Exit(3);
  }
}
}  // namespace

void test_lifecycle_log(const char* format, ...) {
  if (fail_initial_log && std::strncmp(format, "promise: initial_suspend:", 25) == 0) {
    fail_initial_log = false;
    throw creation_failure{};
  }
}

int main(int argc, char** argv) {
  ASSERT(argc == 2);
  if (std::strcmp(argv[1], "initial_void") == 0) {
    test_initial_failure<void>();
    return 0;
  }
  if (std::strcmp(argv[1], "initial_value") == 0) {
    test_initial_failure<int>();
    return 0;
  }
  if (std::strcmp(argv[1], "normal") == 0) {
    {
      auto empty = task<void>(false);
      empty.current_coro_handle_.resume();
      empty.await_resume();
      ASSERT(body_unwound);
      body_unwound = false;
      auto value = task<int>(false);
      value.current_coro_handle_.resume();
      ASSERT(value.await_resume() == 42);
      ASSERT(body_unwound);
    }
    check_coro_leak();
    return 0;
  }
  std::set_terminate(expect_termination);
  if (std::strcmp(argv[1], "void") == 0) {
    auto empty = task<void>(true);
    empty.current_coro_handle_.resume();
  } else {
    ASSERT(std::strcmp(argv[1], "value") == 0);
    auto value = task<int>(true);
    value.current_coro_handle_.resume();
  }
  // Reaching this point means unhandled_exception silently accepted the failure.
  return 1;
}
