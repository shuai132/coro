#define CORO_DEBUG_PROMISE_LEAK
#include "assert_def.h"
#include "coro/coro.hpp"
#include "log.h"
#include "utils.hpp"

#ifdef CORO_DISABLE_EXCEPTION
static_assert(EXPECT_DISABLED);
#else
static_assert(!EXPECT_DISABLED);
#endif

coro::async<int> value() {
  co_return 42;
}

int main() {
  int result;
  {
    auto task = value();
    task.current_coro_handle_.resume();
    result = task.await_resume();
  }
  check_coro_leak();
  return result == 42 ? 0 : 1;
}
