/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include "detail/log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include "coro.hpp"
#include "detail/TimeCount.hpp"
#include "detail/assert_def.h"
#include "detail/utils.hpp"

using namespace coro;

// Simple task that simulates work
async<void> worker_task(wait_group& wg, std::string name, int work_ms) {
  LOG("Worker %s: starting work", name.c_str());
  co_await sleep(std::chrono::milliseconds(work_ms));
  LOG("Worker %s: finished work after %d ms", name.c_str(), work_ms);
  wg.done();  // Signal completion
}

// Test basic wait_group functionality
async<void> wait_group_basic_test() {
  LOG("=== Basic Wait Group Test ===");
  wait_group wg;

  // Add 3 tasks
  wg.add(3);

  auto exec = co_await current_executor();

  // Spawn 3 worker tasks
  co_spawn(*exec, worker_task(wg, "A", 50));
  co_spawn(*exec, worker_task(wg, "B", 100));
  co_spawn(*exec, worker_task(wg, "C", 75));

  LOG("Main: waiting for all workers to complete...");
  co_await wg.wait();
  LOG("Main: all workers completed!");

  ASSERT(wg.get_count() == 0);
}

// Test sequential done() calls
async<void> wait_group_sequential_test() {
  LOG("=== Sequential Wait Group Test ===");
  wait_group wg;

  wg.add(3);
  LOG("Counter after add(3): %d", wg.get_count());
  ASSERT(wg.get_count() == 3);

  wg.done();
  LOG("Counter after done(): %d", wg.get_count());
  ASSERT(wg.get_count() == 2);

  wg.done();
  LOG("Counter after done(): %d", wg.get_count());
  ASSERT(wg.get_count() == 1);

  wg.done();
  LOG("Counter after done(): %d", wg.get_count());
  ASSERT(wg.get_count() == 0);

  // Wait should complete immediately since counter is 0
  co_await wg.wait();
  LOG("Wait completed immediately as expected");
}

// Test multiple waiters
async<void> wait_group_multiple_waiters_test() {
  LOG("=== Multiple Waiters Test ===");
  wait_group wg;

  auto exec = co_await current_executor();

  // Waiter tasks
  auto waiter_task = [&wg](const char* name) -> async<void> {
    LOG("Waiter %s: waiting...", name);
    co_await wg.wait();
    LOG("Waiter %s: wait completed!", name);
  };

  // Add some work
  wg.add(2);

  // Spawn multiple waiters
  co_spawn(*exec, waiter_task("W1"));
  co_spawn(*exec, waiter_task("W2"));
  co_spawn(*exec, waiter_task("W3"));

  // Let waiters start
  co_await sleep(10ms);

  LOG("Main: completing work item 1");
  wg.done();
  co_await sleep(10ms);

  LOG("Main: completing work item 2");
  wg.done();

  // Give time for all waiters to wake up
  co_await sleep(50ms);
  LOG("Main: all waiters should have completed");
}

// Test incremental add
async<void> wait_group_incremental_add_test() {
  LOG("=== Incremental Add Test ===");
  wait_group wg;

  auto exec = co_await current_executor();

  // Add tasks one by one
  for (int i = 0; i < 5; i++) {
    wg.add(1);
    auto name = std::string("Worker-") + std::to_string(i);
    co_spawn(*exec, worker_task(wg, name.c_str(), 20 + i * 10));
  }

  LOG("Main: added 5 tasks, waiting...");
  co_await wg.wait();
  LOG("Main: all 5 tasks completed!");
}

// Test with negative counter (add negative value)
async<void> wait_group_negative_delta_test() {
  LOG("=== Negative Delta Test ===");
  wait_group wg;

  wg.add(10);
  LOG("Counter after add(10): %d", wg.get_count());
  ASSERT(wg.get_count() == 10);

  wg.add(-5);
  LOG("Counter after add(-5): %d", wg.get_count());
  ASSERT(wg.get_count() == 5);

  wg.add(-5);
  LOG("Counter after add(-5): %d", wg.get_count());
  ASSERT(wg.get_count() == 0);

  co_await wg.wait();
  LOG("Wait completed!");
}

// Test concurrent workers with different completion times
async<void> wait_group_concurrent_test() {
  LOG("=== Concurrent Workers Test ===");
  wait_group wg;

  auto exec = co_await current_executor();

  const int num_workers = 10;
  wg.add(num_workers);

  TimeCount t;

  // Spawn workers with varying work times
  for (int i = 0; i < num_workers; i++) {
    auto name = std::string("W") + std::to_string(i);
    int work_time = 10 + (i % 3) * 20;  // 10, 30, 50, 10, 30...
    co_spawn(*exec, worker_task(wg, name, work_time));
  }

  LOG("Main: waiting for %d workers...", num_workers);
  co_await wg.wait();
  auto elapsed = t.elapsed();
  LOG("Main: all %d workers completed in %d ms", num_workers, (int)elapsed);

  ASSERT(wg.get_count() == 0);
}

// Test reusing wait_group
async<void> wait_group_reuse_test() {
  LOG("=== Reuse Wait Group Test ===");
  wait_group wg;

  auto exec = co_await current_executor();

  // First batch
  LOG("First batch: adding 3 tasks");
  wg.add(3);
  co_spawn(*exec, worker_task(wg, "Batch1-A", 20));
  co_spawn(*exec, worker_task(wg, "Batch1-B", 20));
  co_spawn(*exec, worker_task(wg, "Batch1-C", 20));
  co_await wg.wait();
  LOG("First batch completed");

  // Second batch
  LOG("Second batch: adding 2 tasks");
  wg.add(2);
  co_spawn(*exec, worker_task(wg, "Batch2-A", 20));
  co_spawn(*exec, worker_task(wg, "Batch2-B", 20));
  co_await wg.wait();
  LOG("Second batch completed");

  LOG("Wait group successfully reused!");
}

async<void> run_all_tests() {
  co_await wait_group_basic_test();
  LOG("");

  co_await wait_group_sequential_test();
  LOG("");

  co_await wait_group_multiple_waiters_test();
  LOG("");

  co_await wait_group_incremental_add_test();
  LOG("");

  co_await wait_group_negative_delta_test();
  LOG("");

  co_await wait_group_concurrent_test();
  LOG("");

  co_await wait_group_reuse_test();
  LOG("");

  LOG("=== All Wait Group Tests Passed! ===");
}

int main() {
  LOG("Wait Group test init");
  executor_loop executor;
  run_all_tests().detach_with_callback(executor, [&] {
    executor.stop();
  });
  LOG("loop...");
  executor.run_loop();
  check_coro_leak();
  return 0;
}
