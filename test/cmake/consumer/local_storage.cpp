#define CORO_DEBUG_PROMISE_LEAK

#include "assert_def.h"
#include "coro/coro_local.hpp"
#include "log.h"
#include "utils.hpp"

coro::async<int> read_local_values() {
  coro::coro_local<int> value;
  auto first = co_await value.get();
  auto second = co_await value.get_optional();
  co_return first + second.value_or(0);
}

int main() {
  {
    auto task = read_local_values();
    task.current_coro_handle_.resume();
    ASSERT(task.current_coro_handle_.done());
    ASSERT(task.await_resume() == 0);
  }
  check_coro_leak();
}
