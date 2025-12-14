/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include "log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include "TimeCount.hpp"
#include "assert_def.h"
#include "coro.hpp"

/// config executor
#define CORO_EXECUTOR_SINGLE_THREAD
#define CORO_EXECUTOR_POLL

using namespace coro;

callback_awaiter<void> delay_ms(uint32_t ms) {
  return callback_awaiter<void>([ms](auto executor, auto callback) {
    executor->post_delayed(std::move(callback), ms);
  });
}

async<void> mutex_test_task(executor& exec, coro::mutex& mtx, const char* name, int delay_duration) {
  LOG("Task %s: attempting to acquire lock", name);
  {
    auto guard = co_await mtx.lock();
    LOG("Task %s: acquired lock", name);

    // Simulate some work holding the lock
    TimeCount t;
    co_await delay_ms(delay_duration);
    LOG("Task %s: finished work after %d ms", name, (int)t.elapsed());

    // Lock is released automatically when guard goes out of scope
  }
  LOG("Task %s: released lock", name);
}

async<void> mutex_test_sequential_tasks(executor& exec) {
  coro::mutex mtx;

  // Test with sequential acquisition of the lock
  LOG("Sequential mutex test: starting...");

  // Run task A
  {
    auto guard = co_await mtx.lock();
    LOG("Task A: acquired lock");
    co_await delay_ms(500);
    LOG("Task A: finished work");
  }
  LOG("Task A: released lock");

  // Run task B
  {
    auto guard = co_await mtx.lock();
    LOG("Task B: acquired lock");
    co_await delay_ms(300);
    LOG("Task B: finished work");
  }
  LOG("Task B: released lock");

  // Run task C
  {
    auto guard = co_await mtx.lock();
    LOG("Task C: acquired lock");
    co_await delay_ms(700);
    LOG("Task C: finished work");
  }
  LOG("Task C: released lock");

  LOG("Sequential mutex test: completed");
}

async<void> mutex_test_concurrent_tasks(executor& exec) {
  coro::mutex mtx;

  // Launch multiple tasks that compete for the same mutex
  co_spawn(exec, mutex_test_task(exec, mtx, "A", 500));
  co_spawn(exec, mutex_test_task(exec, mtx, "B", 300));
  co_spawn(exec, mutex_test_task(exec, mtx, "C", 700));

  // Wait a bit more to ensure all tasks complete
  co_await delay_ms(2000);
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
    co_await delay_ms(10);
    LOG("Work completed with lock held: OK");

    // The lock is automatically released when guard goes out of scope
  }
  LOG("Mutex unlocked automatically when guard went out of scope: OK");

  // At this point, the lock should be unlocked
  ASSERT(!mtx.is_locked());
  LOG("Mutex finally unlocked: OK");
}

async<void> mutex_race_condition_test(executor& exec, int& shared_counter) {
  coro::mutex mtx;

  auto increment_task = [&mtx, &shared_counter](int increments) -> async<void> {
    for (int i = 0; i < increments; ++i) {
      {
        auto guard = co_await mtx.lock();
        // Critical section
        int temp = shared_counter;
        co_await delay_ms(1);  // Small delay to increase chance of race condition if not properly locked
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
  co_await delay_ms(1000);

  LOG("Race condition test - final counter value: %d", shared_counter);
  ASSERT(shared_counter == 15);  // Should be exactly 15 (5+5+5) if mutex works correctly
  LOG("Race condition test passed: OK");
}

void debug_and_stop(auto& executor, int wait_ms = 3000) {
  std::thread([&executor, wait_ms] {
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    executor.dispatch([&executor] {
#ifdef CORO_DEBUG_PROMISE_LEAK
      LOG("debug: debug_coro_leak.size: %zu", debug_coro_promise::debug_coro_leak.size());
      ASSERT(debug_coro_promise::debug_coro_leak.empty());
#endif
      executor.stop();
    });
  }).detach();
}

async<void> run_all_tests(executor& exec) {
  // Run basic functionality test first
  co_await mutex_basic_test();
  LOG("Basic test completed");

  // Run sequential test
  co_await mutex_test_sequential_tasks(exec);
  LOG("Sequential test completed");

  // Better concurrent test
  coro::mutex test_mtx;
  co_spawn(exec, mutex_test_task(exec, test_mtx, "1", 300));
  co_spawn(exec, mutex_test_task(exec, test_mtx, "2", 200));
  co_spawn(exec, mutex_test_task(exec, test_mtx, "3", 100));

  // Wait for concurrent tests to complete
  co_await delay_ms(1000);

  // Run race condition test
  int shared_counter = 0;  // Define here to ensure lifetime
  co_await mutex_race_condition_test(exec, shared_counter);
  LOG("Race condition test completed");
}

int main() {
  LOG("Mutex test init");
  executor_single_thread executor;
  co_spawn(executor, run_all_tests(executor));
  debug_and_stop(executor, 6000);
  LOG("loop...");
  executor.run_loop();
  return 0;
}
