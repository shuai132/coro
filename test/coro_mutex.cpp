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
    auto guard = co_await mtx.scoped_lock();
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
    auto guard = co_await mtx.scoped_lock();
    LOG("Task A: acquired lock");
    co_await sleep(10ms);
    LOG("Task A: finished work");
    ASSERT(mtx.is_locked());
  }
  LOG("Task A: released lock");
  ASSERT(!mtx.is_locked());

  // Run task B
  {
    co_await mtx.lock();
    LOG("Task B: acquired lock");
    co_await sleep(10ms);
    LOG("Task B: finished work");
    ASSERT(mtx.is_locked());
    mtx.unlock();
    ASSERT(!mtx.is_locked());
  }
  LOG("Task B: released lock");

  // Run task C
  {
    auto guard = co_await mtx.scoped_lock();
    LOG("Task C: acquired lock");
    co_await sleep(10ms);
    LOG("Task C: finished work");
    guard.unlock();
    ASSERT(!mtx.is_locked());
  }
  LOG("Task C: released lock");

  LOG("Sequential mutex test: completed");
}

async<void> mutex_test_concurrent_tasks() {
  coro::mutex mtx;

  // Launch multiple tasks that compete for the same mutex
  auto exec = co_await current_executor();
  co_spawn(*exec, mutex_test_task(mtx, "A", 10));
  co_spawn(*exec, mutex_test_task(mtx, "B", 10));
  co_spawn(*exec, mutex_test_task(mtx, "C", 10));

  // Wait a bit more to ensure all tasks complete
  co_await sleep(200ms);
  LOG("All concurrent mutex test tasks should be complete");
}

async<void> mutex_basic_test() {
  coro::mutex mtx;

  // Initially unlocked
  ASSERT(!mtx.is_locked());
  LOG("Mutex initially unlocked: OK");

  // Acquire the lock using RAII-style guard
  {
    auto guard = co_await mtx.scoped_lock();
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

async<void> mutex_race_condition_test() {
  coro::mutex mtx;
  int shared_counter = 0;
  auto increment_task = [&mtx, &shared_counter](const char* name, int increments) -> async<void> {
    TimeCount t;
    LOG("increment_task %s: begin", name);
    for (int i = 0; i < increments; ++i) {
      {
        auto guard = co_await mtx.scoped_lock();
        // Critical section
        int temp = shared_counter;
        TimeCount tt;
        co_await sleep(1ms);  // Small delay to increase chance of race condition if not properly locked
        shared_counter = temp + 1;
        LOG("increment_task %s: i:%d, sleep elapsed: %d ms", name, i, (int)tt.elapsed());  // Some platform will slow
        // Lock is released automatically when guard goes out of scope
      }
    }
    LOG("increment_task %s: end: %d ms", name, (int)t.elapsed());
  };

  // Run multiple tasks concurrently that increment the counter
  TimeCount t;
  auto& exec = *co_await current_executor();
  increment_task("A", 10).detach_with_callback(exec, [&] {
    LOG("finish increment_task 1 after: %d", (int)t.elapsed());
  });
  increment_task("B", 10).detach_with_callback(exec, [&] {
    LOG("finish increment_task 2 after: %d", (int)t.elapsed());
  });
  increment_task("C", 10).detach_with_callback(exec, [&] {
    LOG("finish increment_task 3 after: %d", (int)t.elapsed());
  });

  // Wait for all tasks to complete
  co_await sleep(1000ms);  // GitHub ci slow on macOS

  LOG("Race condition test - final counter value: %d", shared_counter);
  ASSERT(shared_counter == 30);
  LOG("Race condition test passed: OK");
}

async<void> run_all_tests() {
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
    // Run race condition test
    co_await mutex_race_condition_test();
    LOG("Race condition test completed");
  }

  {
    co_await mutex_test_concurrent_tasks();
    LOG("Concurrent test completed");
  }
}

int main() {
  LOG("Mutex test init");
  executor_loop executor;
  run_all_tests().detach_with_callback(executor, [&] {
    executor.stop();
  });
  LOG("loop...");
  executor.run_loop();
  check_coro_leak();
  return 0;
}
