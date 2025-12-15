/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include "log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include "TimeCount.hpp"
#include "assert_def.h"
#include "coro.hpp"
#include "utils.hpp"

using namespace coro;

async<void> mutex_test_task(coro::mutex& mtx, const char* name, int sleep_ms) {
  LOG("Task %s: attempting to acquire lock", name);
  {
    auto guard = co_await mtx.lock();
    LOG("Task %s: acquired lock", name);

    // Simulate some work holding the lock
    TimeCount t;
    co_await sleep(std::chrono::milliseconds(sleep_ms));
    LOG("Task %s: finished work after %d ms", name, (int)t.elapsed());

    // Lock is released automatically when guard goes out of scope
  }
  LOG("Task %s: released lock", name);
}

async<void> mutex_test_sequential_tasks() {
  coro::mutex mtx;

  // Test with sequential acquisition of the lock
  LOG("Sequential mutex test: starting...");

  // Run task A
  {
    auto guard = co_await mtx.lock();
    LOG("Task A: acquired lock");
    co_await sleep(10ms);
    LOG("Task A: finished work");
  }
  LOG("Task A: released lock");

  // Run task B
  {
    auto guard = co_await mtx.lock();
    LOG("Task B: acquired lock");
    co_await sleep(20ms);
    LOG("Task B: finished work");
  }
  LOG("Task B: released lock");

  // Run task C
  {
    auto guard = co_await mtx.lock();
    LOG("Task C: acquired lock");
    co_await sleep(30ms);
    LOG("Task C: finished work");
  }
  LOG("Task C: released lock");

  LOG("Sequential mutex test: completed");
}

async<void> mutex_test_concurrent_tasks(executor& exec) {
  coro::mutex mtx;

  // Launch multiple tasks that compete for the same mutex
  co_spawn(exec, mutex_test_task(mtx, "A", 10));
  co_spawn(exec, mutex_test_task(mtx, "B", 10));
  co_spawn(exec, mutex_test_task(mtx, "C", 10));

  // Wait a bit more to ensure all tasks complete
  co_await sleep(50ms);
  LOG("All concurrent mutex test tasks should be complete");
}

async<void> mutex_basic_test() {
  coro::mutex mtx;

  // Initially unlocked
  ASSERT(!mtx.is_locked());
  LOG("Mutex initially unlocked: OK");

  // Acquire the lock using RAII-style guard
  {
    auto guard = co_await mtx.lock();
    LOG("Mutex acquired: OK");

    // Do a small amount of work while holding the lock
    co_await sleep(10ms);
    LOG("Work completed with lock held: OK");

    // The lock is automatically released when guard goes out of scope
  }
  LOG("Mutex unlocked automatically when guard went out of scope: OK");

  // At this point, the lock should be unlocked
  ASSERT(!mtx.is_locked());
  LOG("Mutex finally unlocked: OK");
}

async<int> mutex_race_condition_test(executor& exec) {
  coro::mutex mtx;
  int shared_counter = 0;
  auto increment_task = [&mtx, &shared_counter](int increments) -> async<void> {
    for (int i = 0; i < increments; ++i) {
      {
        auto guard = co_await mtx.lock();
        // Critical section
        int temp = shared_counter;
        co_await sleep(1ms);  // Small delay to increase chance of race condition if not properly locked
        shared_counter = temp + 1;
        // Lock is released automatically when guard goes out of scope
      }
    }
  };

  // Run multiple tasks concurrently that increment the counter
  co_spawn(exec, increment_task(5));
  co_spawn(exec, increment_task(5));
  co_spawn(exec, increment_task(5));

  // Wait for all tasks to complete
  co_await sleep(100ms);

  LOG("Race condition test - final counter value: %d", shared_counter);
  ASSERT(shared_counter == 15);  // Should be exactly 15 (5+5+5) if mutex works correctly
  LOG("Race condition test passed: OK");
  co_return shared_counter;
}

async<void> run_all_tests(executor& exec) {
  {
    // Run basic functionality test first
    co_await mutex_basic_test();
    LOG("Basic test completed");
  }

  {
    // Run sequential test
    co_await mutex_test_sequential_tasks();
    LOG("Sequential test completed");
  }

  {
    // Better concurrent test
    coro::mutex test_mtx;
    co_spawn(exec, mutex_test_task(test_mtx, "1", 10));
    co_spawn(exec, mutex_test_task(test_mtx, "2", 10));
    co_spawn(exec, mutex_test_task(test_mtx, "3", 10));

    // Wait for concurrent tests to complete
    co_await sleep(50ms);
    LOG("Better concurrent test completed");
  }

  {
    // Run race condition test
    int shared_counter = co_await mutex_race_condition_test(exec);
    ASSERT(shared_counter == 15);
    LOG("Race condition test completed");
  }

  {
    co_await mutex_test_concurrent_tasks(exec);
    LOG("Concurrent test completed");
  }
}

int main() {
  LOG("Mutex test init");
  executor_loop executor;
  co_spawn(executor, run_all_tests(executor));
  auto debug = debug_and_stop(executor, 1000);
  LOG("loop...");
  executor.run_loop();
  debug.join();
  return 0;
}
