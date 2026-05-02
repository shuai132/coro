#define CORO_DEBUG_PROMISE_LEAK

#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <type_traits>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "log.h"
#include "utils.hpp"

namespace {
bool body_unwound = false;

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

int main(int argc, char** argv) {
  ASSERT(argc == 2);
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
