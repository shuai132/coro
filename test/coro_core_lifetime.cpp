#define CORO_DEBUG_PROMISE_LEAK

#include "assert_def.h"
#include "coro/coro.hpp"

using namespace coro;

async<int> lazy_value() {
  co_return 42;
}

int main() {
  ASSERT(debug_coro_promise::debug_coro_leak.empty());

  {
    auto task = lazy_value();
    (void)task;
  }

  ASSERT(debug_coro_promise::debug_coro_leak.empty());
  return 0;
}
