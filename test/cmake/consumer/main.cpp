#define CORO_DEBUG_PROMISE_LEAK

#include "assert_def.h"
#include "coro/coro.hpp"
#include "log.h"
#include "utils.hpp"

coro::async<int> value() {
  co_return 42;
}

int main() {
  {
    auto task = value();
    task.current_coro_handle_.resume();
    ASSERT(task.current_coro_handle_.done());
    ASSERT(task.await_resume() == 42);
  }
  check_coro_leak();
  return 0;
}
