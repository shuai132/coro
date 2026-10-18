/// config debug
#define CORO_DEBUG_PROMISE_LEAK
#include <thread>

#include "coro.hpp"
#include "detail/assert_def.h"
#include "detail/log.h"
#include "detail/utils.hpp"
#include "exception_mutex.hpp"

using namespace coro;

void test_latch_lock_failure() {
  static_assert(!noexcept(std::declval<latch&>().try_wait()));
  static_assert(noexcept(std::declval<latch_st&>().try_wait()));
#ifndef CORO_DISABLE_EXCEPTION
  latch_t<exception_mutex> latch(0);
  expect_lock_failure([&] {
    (void)latch.try_wait();
  });
  ASSERT(latch.try_wait());
#endif
}

async<void> wait_for_latch_waiters(const int& completed, int expected) {
  auto deadline = std::chrono::steady_clock::now() + 5s;
  while (completed != expected && std::chrono::steady_clock::now() < deadline) {
    co_await sleep(1ms);
  }
  ASSERT(completed == expected);
}

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

  co_await spawn_local(waiter());
  co_await spawn_local(waiter());
  co_await spawn_local(waiter());

  ASSERT(completed == 0);
  auto* exec = co_await current_executor();
  exec->post([] {
    // Exercise queued wakeups when the executor is busy longer than the old readiness delay.
    std::this_thread::sleep_for(20ms);
  });
  ltc.count_down(3);
  co_await wait_for_latch_waiters(completed, 3);
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

  co_await spawn_local(worker());
  co_await spawn_local(worker());
  co_await spawn_local(worker());

  co_await wait_for_latch_waiters(completed, 3);
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
  ltc.count_down(0);
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
  test_latch_lock_failure();
  LOG("Latch test init");
  executor_loop executor;
  run_all_tests().detach(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  check_coro_leak();
  return 0;
}
