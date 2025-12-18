/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include "log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include <atomic>
#include <thread>

#include "TimeCount.hpp"
#include "assert_def.h"
#include "coro.hpp"
#include "utils.hpp"

using namespace coro;

// Coroutine task that acquires mutex and modifies shared counter
// This task will run on its own executor in a separate thread
async<void> mutex_task(coro::mutex& mtx, std::atomic<int>& shared_counter, int task_id) {
  LOG("Task %d: started", task_id);

  for (int i = 0; i < 10; ++i) {
    // Acquire the mutex
    auto guard = co_await mtx.scoped_lock();

    // Critical section: read-modify-write on shared counter
    int temp = shared_counter.load();
    LOG("Task %d: iteration %d, got lock, counter = %d", task_id, i, temp);

    // Simulate some work with the lock held
    co_await sleep(5ms);

    // Increment the counter
    shared_counter.store(temp + 1);
    LOG("Task %d: iteration %d, incremented counter to %d", task_id, i, shared_counter.load());

    // Lock is automatically released when guard goes out of scope
  }

  LOG("Task %d: finished", task_id);
}

async<void> run_test(executor& exec1, executor& exec2) {
  LOG("=== Testing Mutex Across Threads ===");

  // Create a shared mutex and counter
  coro::mutex mtx;
  std::atomic<int> shared_counter{0};
  TimeCount t;

  // Launch two tasks on different executors (different threads)
  auto task1 = mutex_task(mtx, shared_counter, 1).bind_executor(exec1);
  auto task2 = mutex_task(mtx, shared_counter, 2).bind_executor(exec2);

  // Start test
#if 1
  co_await when_all(std::move(task1), std::move(task2));
#else
  co_spawn(exec1, std::move(task1));
  co_spawn(exec2, std::move(task2));
  co_await sleep(1s);
#endif

  // Verify the result
  int final_count = shared_counter.load();
  LOG("Final counter value: %d (expected 20), elapsed: %d", final_count, (int)t.elapsed());
  ASSERT(final_count == 20);

  LOG("=== Mutex Multi-Thread Test PASSED ===");
}

int main() {
  LOG("Multi-thread test init");

  executor_loop exec1;
  executor_loop exec2;

  // Thread 1: Run executor 1
  std::thread thread1([&] {
    LOG("Thread 1: starting executor loop");
    exec1.run_loop();
    LOG("Thread 1: executor loop stopped");
  });

  // Thread 2: Run executor 2
  std::thread thread2([&] {
    LOG("Thread 2: starting executor loop");
    exec2.run_loop();
    LOG("Thread 2: executor loop stopped");
  });

  // Start the test on executor 1
  run_test(exec1, exec2).detach_with_callback(exec1, [&] {
    LOG("Test completed, stopping executors");
    exec1.stop();
    exec2.stop();
  });

  // Wait for both executors to finish
  thread1.join();
  thread2.join();
  LOG("Test completed successfully");

  check_coro_leak();
  return 0;
}
