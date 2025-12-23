/// config debug
#define CORO_DEBUG_PROMISE_LEAK
#include "coro.hpp"
#include "detail/assert_def.h"
#include "detail/log.h"
#include "detail/utils.hpp"

using namespace coro;

// Test 1: Basic event - set wakes all waiters
async<void> event_basic_test() {
  LOG("=== Basic Event Test ===");
  coro::event evt;

  ASSERT(!evt.is_set());

  int completed = 0;
  auto waiter = [&]() -> async<void> {
    co_await evt;
    completed++;
    co_return;
  };

  auto exec = co_await current_executor();
  co_spawn(*exec, waiter());
  co_spawn(*exec, waiter());
  co_spawn(*exec, waiter());

  co_await sleep(10ms);
  ASSERT(completed == 0);

  LOG("Setting event...");
  evt.set();
  ASSERT(evt.is_set());

  co_await sleep(10ms);
  ASSERT(completed == 3);

  LOG("Basic test: OK");
  co_return;
}

// Test 2: Event already set - no suspend
async<void> event_already_set_test() {
  LOG("=== Event Already Set Test ===");
  coro::event evt;

  evt.set();
  ASSERT(evt.is_set());

  // These should all return immediately without suspending
  int completed = 0;
  auto waiter = [&]() -> async<void> {
    co_await evt;
    completed++;
    co_return;
  };

  auto exec = co_await current_executor();
  co_spawn(*exec, waiter());
  co_spawn(*exec, waiter());
  co_spawn(*exec, waiter());

  co_await sleep(10ms);
  ASSERT(completed == 3);

  LOG("Already set test: OK");
  co_return;
}

// Test 3: Clear and re-set
async<void> event_clear_reset_test() {
  LOG("=== Clear and Reset Test ===");
  coro::event evt;

  evt.set();
  ASSERT(evt.is_set());

  // Should proceed immediately
  co_await evt;

  LOG("Clearing event...");
  evt.clear();
  ASSERT(!evt.is_set());

  // This should block
  int completed = 0;
  auto waiter = [&]() -> async<void> {
    co_await evt;
    completed++;
    co_return;
  };

  auto exec = co_await current_executor();
  co_spawn(*exec, waiter());

  co_await sleep(10ms);
  ASSERT(completed == 0);

  LOG("Setting event again...");
  evt.set();
  co_await sleep(10ms);
  ASSERT(completed == 1);

  LOG("Clear and reset test: OK");
  co_return;
}

// Test 4: Direct co_await on event
async<void> event_direct_await_test() {
  LOG("=== Direct co_await Test ===");
  coro::event evt;

  int completed = 0;
  auto task = [&]() -> async<void> {
    co_await evt;  // Direct co_await without .wait()
    completed++;
    co_return;
  };

  auto exec = co_await current_executor();
  co_spawn(*exec, task());

  co_await sleep(10ms);
  ASSERT(completed == 0);

  evt.set();
  co_await sleep(10ms);
  ASSERT(completed == 1);

  LOG("Direct await test: OK");
  co_return;
}

// Test 5: Multiple set/clear cycles
async<void> event_multiple_cycles_test() {
  LOG("=== Multiple Cycles Test ===");
  coro::event evt;

  for (int cycle = 0; cycle < 3; ++cycle) {
    LOG("Cycle %d", cycle + 1);

    int completed = 0;
    auto waiter = [&]() -> async<void> {
      co_await evt;
      completed++;
      co_return;
    };

    auto exec = co_await current_executor();
    co_spawn(*exec, waiter());
    co_spawn(*exec, waiter());

    co_await sleep(10ms);
    ASSERT(completed == 0);

    evt.set();
    co_await sleep(10ms);
    ASSERT(completed == 2);

    evt.clear();
    ASSERT(!evt.is_set());
  }

  LOG("Multiple cycles test: OK");
  co_return;
}

// Test 6: Many waiters stress test
async<void> event_many_waiters_test() {
  LOG("=== Many Waiters Stress Test ===");
  coro::event evt;

  constexpr int NUM_WAITERS = 100;
  int completed = 0;

  auto waiter = [&]() -> async<void> {
    co_await evt;
    completed++;
    co_return;
  };

  auto exec = co_await current_executor();
  for (int i = 0; i < NUM_WAITERS; ++i) {
    co_spawn(*exec, waiter());
  }

  co_await sleep(10ms);
  ASSERT(completed == 0);

  LOG("Setting event to wake %d waiters...", NUM_WAITERS);
  evt.set();
  co_await sleep(50ms);
  ASSERT(completed == NUM_WAITERS);

  LOG("Many waiters test: OK");
  co_return;
}

async<void> run_all_tests() {
  co_await event_basic_test();
  co_await event_already_set_test();
  co_await event_clear_reset_test();
  co_await event_direct_await_test();
  co_await event_multiple_cycles_test();
  co_await event_many_waiters_test();

  LOG("\n=== All Event Tests Passed ===");
  co_return;
}

int main() {
  LOG("Event test init");
  executor_loop executor;
  run_all_tests().detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  check_coro_leak();
  return 0;
}
