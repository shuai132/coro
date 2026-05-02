#define CORO_DEBUG_PROMISE_LEAK

#include <cstdint>
#include <limits>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "log.h"
#include "utils.hpp"

extern "C" int c_value(void);

#if defined(__GNUC__)
static std::uintptr_t lowest_stack = std::numeric_limits<std::uintptr_t>::max();
static std::uintptr_t highest_stack = 0;

__attribute__((noinline)) void record_stack() {
  volatile char marker;
  auto address = reinterpret_cast<std::uintptr_t>(&marker);
  if (address < lowest_stack) lowest_stack = address;
  if (address > highest_stack) highest_stack = address;
}
#endif

coro::async<int> value() {
#if defined(__GNUC__)
  record_stack();
#endif
  co_return c_value();
}

coro::async<int> repeated_values() {
  int result = 0;
  for (int i = 0; i < 512; ++i) {
    result = co_await value();
  }
  co_return result;
}

int main() {
  {
    auto task = repeated_values();
    task.current_coro_handle_.resume();
    ASSERT(task.current_coro_handle_.done());
    ASSERT(task.await_resume() == 42);
  }
#if defined(__GNUC__)
  // Sequential awaits must not accumulate native stack in a consuming CMake target.
  ASSERT(highest_stack - lowest_stack < 4096);
#endif
  check_coro_leak();
  return 0;
}
