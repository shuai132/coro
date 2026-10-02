#define CORO_DEBUG_PROMISE_LEAK
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
    std::_Exit(0);
  } catch (...) {
    std::_Exit(5);
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

}  // namespace

int main(int argc, char** argv) {
  ASSERT(argc == 2);
  if (std::strcmp(argv[1], "normal") == 0) {
    test_normal_completion<void>();
    body_unwound = false;
    test_normal_completion<int>();
    return 0;
  }
  std::set_terminate(expect_termination);
  expected_body_unwound = true;
  auto token = std::make_shared<int>(0);
  if (std::strcmp(argv[1], "void") == 0) {
    auto value = task<void>(true, token);
    value.current_coro_handle_.resume();
  } else {
    ASSERT(std::strcmp(argv[1], "value") == 0);
    auto value = task<int>(true, token);
    value.current_coro_handle_.resume();
  }
  // An unhandled coroutine-body exception must terminate in disabled-exception mode.
  return 1;
}
