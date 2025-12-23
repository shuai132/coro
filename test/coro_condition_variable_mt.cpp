/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include "log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include <atomic>
#include <thread>
#include <vector>

#include "assert_def.h"
#include "coro.hpp"
#include "utils.hpp"

using namespace coro;
using namespace std::chrono_literals;

// Test concurrent notify_one from multiple threads
void test_concurrent_notify() {
  LOG("=== Testing concurrent notify_one ===");

  coro::mutex mtx;
  coro::condition_variable cv;
  std::atomic<int> wakeup_count{0};
  const int num_waiters = 10;
  const int num_notifiers = 3;

  // Create multiple executors in different threads
  std::vector<std::thread> threads;
  std::vector<std::unique_ptr<executor_loop>> executors;

  // Waiter threads - each has its own executor
  for (int i = 0; i < num_waiters; i++) {
    auto exec = std::make_unique<executor_loop>();
    auto* exec_ptr = exec.get();

    // Create waiter with proper parameter passing
    auto waiter = [](coro::mutex& mtx, coro::condition_variable& cv, std::atomic<int>& wakeup_count, int id) -> async<void> {
      LOG("Waiter %d: starting", id);
      co_await mtx.lock();
      LOG("Waiter %d: acquired lock, waiting", id);
      co_await cv.wait(mtx);
      LOG("Waiter %d: wait returned, trying to reacquire lock", id);
      co_await mtx.lock();
      int count = ++wakeup_count;
      LOG("Waiter %d: woke up, wakeup_count=%d", id, count);
      mtx.unlock();
      LOG("Waiter %d: finished", id);
    };

    waiter(mtx, cv, wakeup_count, i).detach(*exec_ptr);

    // Each executor runs in its own thread
    threads.emplace_back([exec_ptr]() {
      exec_ptr->run_loop();
    });

    executors.push_back(std::move(exec));
  }

  // Give waiters time to start and enter wait state
  std::this_thread::sleep_for(200ms);

  // Notifier threads - multiple threads calling notify_one concurrently
  for (int i = 0; i < num_notifiers; i++) {
    threads.emplace_back([&cv, i]() {
      for (int j = 0; j < num_waiters / num_notifiers; j++) {
        std::this_thread::sleep_for(30ms);
        LOG("Notifier %d: calling notify_one (#%d)", i, j + 1);
        cv.notify_one();
      }
    });
  }

  // Extra notifier for remainder
  threads.emplace_back([&cv]() {
    for (int j = 0; j < num_waiters % num_notifiers; j++) {
      std::this_thread::sleep_for(30ms);
      cv.notify_one();
    }
  });

  // Wait for all notifier threads to finish
  for (size_t i = num_waiters; i < threads.size(); i++) {
    threads[i].join();
  }

  // Give time for all coroutines to finish
  std::this_thread::sleep_for(100ms);

  // Stop all executors
  for (auto& exec : executors) {
    exec->stop();
  }

  // Wait for all executor threads
  for (size_t i = 0; i < num_waiters; i++) {
    threads[i].join();
  }

  LOG("Wakeup count: %d, expected: %d", wakeup_count.load(), num_waiters);
  ASSERT(wakeup_count == num_waiters);
  LOG("=== Concurrent notify test passed ===\n");
}

// Test concurrent wait from multiple threads
void test_concurrent_wait() {
  LOG("=== Testing concurrent wait ===");

  coro::mutex mtx;  // Shared mutex (like Go's sync.Cond)
  coro::condition_variable cv;
  std::atomic<int> ready_count{0};
  const int num_threads = 3;  // Reduced for stability
  const int waiters_per_thread = 2;
  const int total_waiters = num_threads * waiters_per_thread;

  std::vector<std::thread> threads;
  std::vector<std::unique_ptr<executor_loop>> executors;

  // Multiple threads, each with multiple waiters
  for (int tid = 0; tid < num_threads; tid++) {
    auto exec = std::make_unique<executor_loop>();
    auto* exec_ptr = exec.get();

    for (int wid = 0; wid < waiters_per_thread; wid++) {
      int unique_id = tid * waiters_per_thread + wid;

      auto waiter = [](coro::mutex& mtx, coro::condition_variable& cv, std::atomic<int>& ready_count, int unique_id) -> async<void> {
        co_await mtx.lock();
        LOG("Waiter %d: waiting", unique_id);
        co_await cv.wait(mtx);
        co_await mtx.lock();
        int count = ++ready_count;
        LOG("Waiter %d: woke up (count=%d)", unique_id, count);
        mtx.unlock();
      };

      waiter(mtx, cv, ready_count, unique_id).detach(*exec_ptr);
    }

    threads.emplace_back([exec_ptr]() {
      exec_ptr->run_loop();
    });

    executors.push_back(std::move(exec));
  }

  // Give waiters time to start and enter wait state
  LOG("Waiting for waiters to be ready...");
  std::this_thread::sleep_for(500ms);

  // Notify all from main thread
  LOG("Notifying all waiters...");
  cv.notify_all();

  // Give time for all coroutines to finish
  std::this_thread::sleep_for(500ms);

  // Stop all executors
  for (auto& exec : executors) {
    exec->stop();
  }

  // Wait for all executor threads
  for (auto& t : threads) {
    t.join();
  }

  LOG("Ready count: %d, expected: %d", ready_count.load(), total_waiters);
  ASSERT(ready_count == total_waiters);
  LOG("=== Concurrent wait test passed ===\n");
}

int main() {
  LOG("Multi-threaded Condition Variable test init");
  test_concurrent_notify();
  test_concurrent_wait();
  LOG("\n=== All multi-threaded tests passed ===");
  check_coro_leak();
  return 0;
}
