/// config debug
#define CORO_DEBUG_PROMISE_LEAK
#include "coro.hpp"
#include "detail/assert_def.h"
#include "detail/log.h"
#include "detail/utils.hpp"

using namespace coro;

// Test basic latch functionality
async<void> latch_basic_test() {
  LOG("=== Basic Latch Test ===");
  coro::latch ltc(3);

  ASSERT(ltc.get_count() == 3);
  ltc.count_down();
  ASSERT(ltc.get_count() == 2);
  ltc.count_down(2);
  ASSERT(ltc.get_count() == 0);

  // Wait should not block since count is already 0
  co_await ltc.wait();
  co_await ltc;  // Test direct co_await

  LOG("Basic test: OK");
  co_return;
}

// Test latch with multiple waiters
async<void> latch_multiple_waiters_test() {
  LOG("=== Multiple Waiters Test ===");
  coro::latch ltc(3);
  int completed = 0;

  auto waiter = [&]() -> async<void> {
    co_await ltc;
    completed++;
    co_return;
  };

  auto exec = co_await current_executor();
  co_spawn(*exec, waiter());
  co_spawn(*exec, waiter());
  co_spawn(*exec, waiter());

  // Count down after short delay
  co_await sleep(10ms);
  ltc.count_down(3);
  co_await sleep(10ms);

  ASSERT(completed == 3);
  LOG("Multiple waiters test: OK");
  co_return;
}

// Test arrive_and_wait
async<void> latch_arrive_and_wait_test() {
  LOG("=== Arrive and Wait Test ===");
  coro::latch ltc(3);
  int completed = 0;

  auto worker = [&]() -> async<void> {
    co_await ltc.arrive_and_wait();
    completed++;
    co_return;
  };

  auto exec = co_await current_executor();
  co_spawn(*exec, worker());
  co_spawn(*exec, worker());
  co_spawn(*exec, worker());

  co_await sleep(50ms);
  ASSERT(completed == 3);
  LOG("Arrive and wait test: OK");
  co_return;
}

// Test try_wait
async<void> latch_try_wait_test() {
  LOG("=== Try Wait Test ===");
  coro::latch ltc(2);

  ASSERT(!ltc.try_wait());
  ltc.count_down();
  ASSERT(!ltc.try_wait());
  ltc.count_down();
  ASSERT(ltc.try_wait());

  LOG("Try wait test: OK");
  co_return;
}

// Test zero-count latch
async<void> latch_zero_count_test() {
  LOG("=== Zero Count Test ===");
  coro::latch ltc(0);

  ASSERT(ltc.try_wait());
  ASSERT(ltc.get_count() == 0);
  co_await ltc.wait();

  LOG("Zero count test: OK");
  co_return;
}

async<void> run_all_tests() {
  co_await latch_basic_test();
  co_await latch_try_wait_test();
  co_await latch_zero_count_test();
  co_await latch_multiple_waiters_test();
  co_await latch_arrive_and_wait_test();

  LOG("\n=== All Latch Tests Passed ===");
  co_return;
}

int main() {
  LOG("Latch test init");
  executor_loop executor;
  run_all_tests().detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  check_coro_leak();
  return 0;
}
