#define CORO_DEBUG_PROMISE_LEAK

#include "assert_def.h"
#include "coro/coro.hpp"

using namespace coro;

async<int> lazy_value() {
  co_return 42;
}

async<void> child_without_executor(int& value) {
  value = 42;
  co_return;
}

async<void> spawn_local_without_executor(int& value) {
  co_await spawn_local(child_without_executor(value));
}

int main() {
  ASSERT(debug_coro_promise::debug_coro_leak.empty());

  {
    auto task = lazy_value();
    (void)task;
  }

  ASSERT(debug_coro_promise::debug_coro_leak.empty());

  int value = 0;
  {
    auto task = spawn_local_without_executor(value);
    task.current_coro_handle_.resume();
    ASSERT(task.current_coro_handle_.done());
  }

  ASSERT(value == 42);
  ASSERT(debug_coro_promise::debug_coro_leak.empty());
  return 0;
}
