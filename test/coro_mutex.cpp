/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include <memory>
#include <thread>

#include "log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include "TimeCount.hpp"
#include "assert_def.h"
#include "coro.hpp"
#include "utils.hpp"

using namespace coro;

async<void> mutex_destroy_previous_guard(coro::mutex& mtx, std::unique_ptr<coro::mutex::lock_guard>& previous_guard, bool& completed) {
  auto guard = co_await mtx.scoped_lock();
  previous_guard.reset();
  ASSERT(mtx.is_locked());
  completed = true;
}

void mutex_guard_reentrant_destruction_test() {
  for (bool use_executor : {false, true}) {
    coro::mutex mtx;
    executor_poll executor;
    ASSERT(mtx.try_lock());
    auto previous_guard = std::make_unique<coro::mutex::lock_guard>(&mtx);
    bool completed = false;
    auto waiter = mutex_destroy_previous_guard(mtx, previous_guard, completed);
    if (use_executor) {
      spawn(executor, std::move(waiter));
      executor.poll();
    } else {
      waiter.current_coro_handle_.promise().started_ = true;
      waiter.current_coro_handle_.resume();
    }
    ASSERT(!completed);
    previous_guard->unlock();
    if (use_executor) {
      ASSERT(!completed);
      executor.poll();
    }
    ASSERT(completed);
    ASSERT(!previous_guard);
    ASSERT(!mtx.is_locked());
  }
  check_coro_leak();
}

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

  auto* exec = co_await current_executor();
  exec->post([] {
    // A busy loop can make the parent's timer ready before the last worker finishes.
    std::this_thread::sleep_for(250ms);
  });

  co_await when_all(mutex_test_task(mtx, "A", 10), mutex_test_task(mtx, "B", 10), mutex_test_task(mtx, "C", 10));
  ASSERT(!mtx.is_locked());
  LOG("All concurrent mutex test tasks completed");
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

async<void> mutex_scheduled_waiter(coro::mutex& mtx, int& step) {
  auto guard = co_await mtx.scoped_lock();
  ASSERT(step == 1);
  step = 2;
  co_return;
}

async<void> mutex_unlock_schedules_waiter_test() {
  LOG("Mutex unlock schedules waiter test: starting...");

  coro::mutex mtx;
  int step = 0;
  auto guard = co_await mtx.scoped_lock();

  co_await spawn_local(mutex_scheduled_waiter(mtx, step));
  ASSERT(step == 0);

  auto* exec = co_await current_executor();
  exec->post([] {
    std::this_thread::sleep_for(20ms);
  });
  guard.unlock();
  ASSERT(step == 0);

  step = 1;
  auto deadline = std::chrono::steady_clock::now() + 5s;
  while (step != 2) {
    ASSERT(std::chrono::steady_clock::now() < deadline);
    co_await sleep(1ms);
  }

  LOG("Mutex unlock schedules waiter test: OK");
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
  auto a = increment_task("A", 10).with_callback([&] {
    LOG("finish increment_task 1 after: %d", (int)t.elapsed());
  });
  auto b = increment_task("B", 10).with_callback([&] {
    LOG("finish increment_task 2 after: %d", (int)t.elapsed());
  });
  auto c = increment_task("C", 10).with_callback([&] {
    LOG("finish increment_task 3 after: %d", (int)t.elapsed());
  });

  // Wait for all tasks to complete
  co_await when_all(std::move(a), std::move(b), std::move(c));

  LOG("Race condition test: shared_counter: %d, %d ms", shared_counter, (int)t.elapsed());
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
    co_await mutex_unlock_schedules_waiter_test();
    LOG("Unlock scheduling test completed");
  }

  {
    co_await mutex_test_concurrent_tasks();
    LOG("Concurrent test completed");
  }
}

int main() {
  LOG("Mutex test init");
  mutex_guard_reentrant_destruction_test();
  executor_loop executor;
  run_all_tests().detach_with_callback(executor, [&] {
    executor.stop();
  });
  LOG("loop...");
  executor.run_loop();
  check_coro_leak();
  return 0;
}
