/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include "detail/log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include <atomic>
#include <thread>
#include <vector>

#include "coro.hpp"
#include "detail/TimeCount.hpp"
#include "detail/assert_def.h"
#include "detail/utils.hpp"

using namespace coro;

// Worker task that runs on a specific executor/thread
async<void> worker_task_mt(wait_group& wg, int task_id, int work_ms, executor& expected_exec, std::thread::id expected_tid) {
  LOG("Worker %d: starting work on thread", task_id);
  ASSERT(std::this_thread::get_id() == expected_tid);
  ASSERT(co_await current_executor() == &expected_exec);

  co_await sleep(std::chrono::milliseconds(work_ms));

  ASSERT(std::this_thread::get_id() == expected_tid);
  ASSERT(co_await current_executor() == &expected_exec);

  LOG("Worker %d: finished work after %d ms", task_id, work_ms);
  wg.done();

  ASSERT(std::this_thread::get_id() == expected_tid);
}

// Test basic wait_group with multiple threads
async<void> wait_group_basic_mt_test(executor& exec1, executor& exec2, std::thread::id exec1_tid, std::thread::id exec2_tid) {
  LOG("=== Basic Wait Group Multi-Thread Test ===");
  wait_group wg;

  // Add 6 tasks
  wg.add(6);

  TimeCount t;

  // Spawn 3 workers on each executor
  auto task1 = worker_task_mt(wg, 1, 50, exec1, exec1_tid).bind_executor(exec1);
  auto task2 = worker_task_mt(wg, 2, 100, exec2, exec2_tid).bind_executor(exec2);
  auto task3 = worker_task_mt(wg, 3, 75, exec1, exec1_tid).bind_executor(exec1);
  auto task4 = worker_task_mt(wg, 4, 60, exec2, exec2_tid).bind_executor(exec2);
  auto task5 = worker_task_mt(wg, 5, 80, exec1, exec1_tid).bind_executor(exec1);
  auto task6 = worker_task_mt(wg, 6, 90, exec2, exec2_tid).bind_executor(exec2);

  // Spawn all tasks
  co_spawn(exec1, std::move(task1));
  co_spawn(exec2, std::move(task2));
  co_spawn(exec1, std::move(task3));
  co_spawn(exec2, std::move(task4));
  co_spawn(exec1, std::move(task5));
  co_spawn(exec2, std::move(task6));

  LOG("Main: waiting for all workers to complete...");
  co_await wg.wait();
  auto elapsed = t.elapsed();

  LOG("Main: all workers completed in %d ms!", (int)elapsed);
  ASSERT(wg.get_count() == 0);

  LOG("=== Basic Multi-Thread Test PASSED ===");
}

// Test multiple waiters on different threads
async<void> wait_group_multiple_waiters_mt_test(executor& exec1, executor& exec2, std::thread::id exec1_tid, std::thread::id exec2_tid) {
  LOG("=== Multiple Waiters Multi-Thread Test ===");
  wait_group wg;

  std::atomic<int> waiter_completed{0};

  // Waiter tasks on different threads
  auto waiter_task = [&wg, &waiter_completed](int id, executor& expected_exec, std::thread::id expected_tid) -> async<void> {
    LOG("Waiter %d: waiting on thread...", id);
    ASSERT(std::this_thread::get_id() == expected_tid);
    ASSERT(co_await current_executor() == &expected_exec);

    co_await wg.wait();

    ASSERT(std::this_thread::get_id() == expected_tid);
    ASSERT(co_await current_executor() == &expected_exec);
    LOG("Waiter %d: wait completed!", id);
    waiter_completed.fetch_add(1, std::memory_order_release);
  };

  // Add some work
  wg.add(4);

  // Spawn multiple waiters on different threads
  auto waiter1 = waiter_task(1, exec1, exec1_tid).bind_executor(exec1);
  auto waiter2 = waiter_task(2, exec2, exec2_tid).bind_executor(exec2);
  auto waiter3 = waiter_task(3, exec1, exec1_tid).bind_executor(exec1);
  auto waiter4 = waiter_task(4, exec2, exec2_tid).bind_executor(exec2);

  co_spawn(exec1, std::move(waiter1));
  co_spawn(exec2, std::move(waiter2));
  co_spawn(exec1, std::move(waiter3));
  co_spawn(exec2, std::move(waiter4));

  // Let waiters start
  co_await sleep(20ms);

  // Complete work items from different threads
  LOG("Main: completing work items...");
  auto worker1 = worker_task_mt(wg, 10, 30, exec1, exec1_tid).bind_executor(exec1);
  auto worker2 = worker_task_mt(wg, 11, 30, exec2, exec2_tid).bind_executor(exec2);
  auto worker3 = worker_task_mt(wg, 12, 30, exec1, exec1_tid).bind_executor(exec1);
  auto worker4 = worker_task_mt(wg, 13, 30, exec2, exec2_tid).bind_executor(exec2);

  co_await when_all(std::move(worker1), std::move(worker2), std::move(worker3), std::move(worker4));

  // Give time for all waiters to wake up
  co_await sleep(50ms);

  int completed = waiter_completed.load(std::memory_order_acquire);
  LOG("Main: %d waiters completed (expected 4)", completed);
  ASSERT(completed == 4);

  LOG("=== Multiple Waiters Multi-Thread Test PASSED ===");
}

// Test concurrent add/done operations from different threads
async<void> wait_group_concurrent_operations_test(executor& exec1, executor& exec2, std::thread::id exec1_tid, std::thread::id exec2_tid) {
  LOG("=== Concurrent Operations Test ===");
  wait_group wg;

  std::atomic<int> tasks_started{0};
  std::atomic<int> tasks_completed{0};

  // Task that dynamically adds and completes work
  auto dynamic_worker = [&wg, &tasks_started, &tasks_completed](int id, int iterations, executor& expected_exec,
                                                                std::thread::id expected_tid) -> async<void> {
    for (int i = 0; i < iterations; i++) {
      wg.add(1);
      tasks_started.fetch_add(1, std::memory_order_release);

      ASSERT(std::this_thread::get_id() == expected_tid);
      ASSERT(co_await current_executor() == &expected_exec);

      co_await sleep(10ms);

      LOG("Dynamic worker %d: iteration %d completing", id, i);
      wg.done();
      tasks_completed.fetch_add(1, std::memory_order_release);

      ASSERT(std::this_thread::get_id() == expected_tid);
    }

    // Signal completion of this worker
    wg.done();
  };

  // Use sentinel count pattern: add 1 for each worker to prevent premature completion
  // Each worker will call done() when it finishes all its iterations
  wg.add(2);

  TimeCount t;

  // Spawn dynamic workers on different threads
  auto dw1 = dynamic_worker(1, 5, exec1, exec1_tid).bind_executor(exec1);
  auto dw2 = dynamic_worker(2, 5, exec2, exec2_tid).bind_executor(exec2);

  co_spawn(exec1, std::move(dw1));
  co_spawn(exec2, std::move(dw2));

  // Wait for all to complete (including the sentinel counts)
  co_await wg.wait();
  auto elapsed = t.elapsed();

  int started = tasks_started.load(std::memory_order_acquire);
  int completed = tasks_completed.load(std::memory_order_acquire);

  LOG("Concurrent operations: started=%d, completed=%d, elapsed=%d ms", started, completed, (int)elapsed);
  ASSERT(started == 10);
  ASSERT(completed == 10);
  ASSERT(wg.get_count() == 0);

  LOG("=== Concurrent Operations Test PASSED ===");
}

// Test heavy concurrent load
async<void> wait_group_stress_test(executor& exec1, executor& exec2, std::thread::id exec1_tid, std::thread::id exec2_tid) {
  LOG("=== Stress Test ===");
  wait_group wg;

  const int num_workers = 20;
  wg.add(num_workers);

  TimeCount t;

  // Spawn many workers on alternating threads
  for (int i = 0; i < num_workers; i++) {
    executor& exec = (i % 2 == 0) ? exec1 : exec2;
    std::thread::id tid = (i % 2 == 0) ? exec1_tid : exec2_tid;
    int work_time = 10 + (i % 5) * 10;

    auto worker = worker_task_mt(wg, 100 + i, work_time, exec, tid).bind_executor(exec);
    co_spawn(exec, std::move(worker));
  }

  LOG("Main: waiting for %d workers across 2 threads...", num_workers);
  co_await wg.wait();
  auto elapsed = t.elapsed();

  LOG("Main: all %d workers completed in %d ms", num_workers, (int)elapsed);
  ASSERT(wg.get_count() == 0);

  LOG("=== Stress Test PASSED ===");
}

// Test reusing wait_group across multiple batches with different threads
async<void> wait_group_reuse_mt_test(executor& exec1, executor& exec2, std::thread::id exec1_tid, std::thread::id exec2_tid) {
  LOG("=== Reuse Multi-Thread Test ===");
  wait_group wg;

  // First batch - mix of threads
  LOG("First batch: adding 4 tasks");
  wg.add(4);
  co_spawn(exec1, worker_task_mt(wg, 201, 20, exec1, exec1_tid).bind_executor(exec1));
  co_spawn(exec2, worker_task_mt(wg, 202, 20, exec2, exec2_tid).bind_executor(exec2));
  co_spawn(exec1, worker_task_mt(wg, 203, 20, exec1, exec1_tid).bind_executor(exec1));
  co_spawn(exec2, worker_task_mt(wg, 204, 20, exec2, exec2_tid).bind_executor(exec2));
  co_await wg.wait();
  LOG("First batch completed, count=%d", wg.get_count());
  ASSERT(wg.get_count() == 0);

  // Second batch - different distribution
  LOG("Second batch: adding 3 tasks");
  wg.add(3);
  co_spawn(exec1, worker_task_mt(wg, 205, 20, exec1, exec1_tid).bind_executor(exec1));
  co_spawn(exec1, worker_task_mt(wg, 206, 20, exec1, exec1_tid).bind_executor(exec1));
  co_spawn(exec2, worker_task_mt(wg, 207, 20, exec2, exec2_tid).bind_executor(exec2));
  co_await wg.wait();
  LOG("Second batch completed, count=%d", wg.get_count());
  ASSERT(wg.get_count() == 0);

  // Third batch - all on one thread
  LOG("Third batch: adding 2 tasks");
  wg.add(2);
  co_spawn(exec2, worker_task_mt(wg, 208, 20, exec2, exec2_tid).bind_executor(exec2));
  co_spawn(exec2, worker_task_mt(wg, 209, 20, exec2, exec2_tid).bind_executor(exec2));
  co_await wg.wait();
  LOG("Third batch completed, count=%d", wg.get_count());
  ASSERT(wg.get_count() == 0);

  LOG("Wait group successfully reused across threads!");
  LOG("=== Reuse Multi-Thread Test PASSED ===");
}

// Test wait_group_st (single-threaded version) on multiple threads to verify it works
async<void> wait_group_st_test(executor& exec1, executor& exec2, std::thread::id exec1_tid, std::thread::id exec2_tid) {
  LOG("=== Single-Thread WaitGroup Type Test ===");

  // Note: wait_group_st is for single-threaded use, but we test it still works correctly
  // when used properly (only accessed from one thread at a time, protected by executor dispatch)
  wait_group_st wg;

  wg.add(3);

  // All operations happen on exec1's thread via its dispatch mechanism
  auto worker1 = [](wait_group_st& wg, int id) -> async<void> {
    LOG("ST Worker %d: working", id);
    co_await sleep(20ms);
    wg.done();
    LOG("ST Worker %d: done", id);
  }(wg, 1);

  auto worker2 = [](wait_group_st& wg, int id) -> async<void> {
    LOG("ST Worker %d: working", id);
    co_await sleep(20ms);
    wg.done();
    LOG("ST Worker %d: done", id);
  }(wg, 2);

  auto worker3 = [](wait_group_st& wg, int id) -> async<void> {
    LOG("ST Worker %d: working", id);
    co_await sleep(20ms);
    wg.done();
    LOG("ST Worker %d: done", id);
  }(wg, 3);

  co_await when_all(std::move(worker1), std::move(worker2), std::move(worker3));
  co_await wg.wait();

  ASSERT(wg.get_count() == 0);
  LOG("=== Single-Thread WaitGroup Type Test PASSED ===");
}

async<void> run_all_tests(executor& exec1, executor& exec2, std::thread::id exec1_tid, std::thread::id exec2_tid) {
  co_await wait_group_basic_mt_test(exec1, exec2, exec1_tid, exec2_tid);
  LOG("");

  co_await wait_group_multiple_waiters_mt_test(exec1, exec2, exec1_tid, exec2_tid);
  LOG("");

  co_await wait_group_concurrent_operations_test(exec1, exec2, exec1_tid, exec2_tid);
  LOG("");

  co_await wait_group_stress_test(exec1, exec2, exec1_tid, exec2_tid);
  LOG("");

  co_await wait_group_reuse_mt_test(exec1, exec2, exec1_tid, exec2_tid);
  LOG("");

  co_await wait_group_st_test(exec1, exec2, exec1_tid, exec2_tid);
  LOG("");

  LOG("=== All Wait Group Multi-Thread Tests PASSED! ===");
}

int main() {
  LOG("Wait Group Multi-Thread test init");

  executor_loop exec1;
  executor_loop exec2;
  executor_loop exec3;

  // Record thread IDs
  std::thread::id exec1_tid;
  std::thread::id exec2_tid;
  std::thread::id exec3_tid;
  std::atomic<int> exec_tid_ready = 0;

  // Thread 1: Run executor 1
  std::thread thread1([&] {
    exec1_tid = std::this_thread::get_id();
    exec_tid_ready.fetch_add(1, std::memory_order_release);
    LOG("Thread 1: starting executor loop");
    exec1.run_loop();
    LOG("Thread 1: executor loop stopped");
  });

  // Thread 2: Run executor 2
  std::thread thread2([&] {
    exec2_tid = std::this_thread::get_id();
    exec_tid_ready.fetch_add(1, std::memory_order_release);
    LOG("Thread 2: starting executor loop");
    exec2.run_loop();
    LOG("Thread 2: executor loop stopped");
  });

  // Thread 3: Run main test executor
  std::thread thread3([&] {
    exec3_tid = std::this_thread::get_id();
    exec_tid_ready.fetch_add(1, std::memory_order_release);
    LOG("Thread 3: starting executor loop");
    exec3.run_loop();
    LOG("Thread 3: executor loop stopped");
  });

  // Wait for all threads to be ready
  while (exec_tid_ready.load(std::memory_order_acquire) < 3) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Start the tests
  auto test_coro = run_all_tests(exec1, exec2, exec1_tid, exec2_tid);

  test_coro.detach_with_callback(exec3, [&] {
    LOG("Test completed, stopping executors");
    exec1.stop();
    exec2.stop();
    exec3.stop();
  });

  // Wait for all threads to finish
  thread1.join();
  thread2.join();
  thread3.join();

  LOG("All tests completed successfully");

  check_coro_leak();
  return 0;
}
