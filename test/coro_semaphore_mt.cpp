/// config debug
#define CORO_DEBUG_PROMISE_LEAK
#include <atomic>
#include <thread>
#include <vector>

#include "coro.hpp"
#include "detail/assert_def.h"
#include "detail/log.h"
#include "detail/utils.hpp"

using namespace coro;

// Test basic semaphore in multithreaded environment
async<void> semaphore_mt_basic_test(counting_semaphore& sem, std::atomic<int>& counter) {
  LOG("Thread: acquiring semaphore");
  co_await sem.acquire();

  LOG("Thread: acquired, incrementing counter");
  counter++;
  co_await sleep(10ms);

  LOG("Thread: releasing semaphore");
  sem.release();

  co_return;
}

// Test resource pool with multiple threads
async<void> semaphore_mt_resource_pool_test(counting_semaphore& sem, std::atomic<int>& max_concurrent, std::atomic<int>& current_using, int id) {
  co_await sem.acquire();

  int cur = ++current_using;
  int expected = max_concurrent.load();
  while (cur > expected && !max_concurrent.compare_exchange_weak(expected, cur)) {
    expected = max_concurrent.load();
  }

  LOG("Worker %d: using resource (concurrent=%d)", id, cur);
  co_await sleep(20ms);

  current_using--;
  sem.release();

  LOG("Worker %d: released", id);
  co_return;
}

// Test producer-consumer pattern
async<void> producer(counting_semaphore& empty_slots, counting_semaphore& filled_slots, std::atomic<int>& produced, int count) {
  for (int i = 0; i < count; i++) {
    co_await empty_slots.acquire();  // Wait for empty slot
    produced++;
    LOG("Produced item #%d (total=%d)", i + 1, produced.load());
    filled_slots.release();  // Signal filled slot
    co_await sleep(5ms);
  }
  co_return;
}

async<void> consumer(counting_semaphore& empty_slots, counting_semaphore& filled_slots, std::atomic<int>& consumed, int count) {
  for (int i = 0; i < count; i++) {
    co_await filled_slots.acquire();  // Wait for filled slot
    consumed++;
    LOG("Consumed item #%d (total=%d)", i + 1, consumed.load());
    empty_slots.release();  // Signal empty slot
    co_await sleep(5ms);
  }
  co_return;
}

// Test binary semaphore as mutex alternative
async<void> semaphore_mt_binary_test(binary_semaphore& sem, std::atomic<int>& counter, std::atomic<int>& max_concurrent, int iterations) {
  for (int i = 0; i < iterations; i++) {
    co_await sem.acquire();

    // Critical section
    int old_val = counter++;
    if (counter > max_concurrent) {
      max_concurrent = counter.load();
    }

    ASSERT(counter == old_val + 1);  // Should be exactly old_val + 1

    co_await sleep(1ms);
    counter--;

    sem.release();
  }
  co_return;
}

void run_basic_test() {
  LOG("=== Multi-threaded Basic Test ===");

  counting_semaphore sem(3);
  std::atomic<int> counter{0};
  std::atomic<int> completed{0};

  const int THREAD_COUNT = 4;
  const int TASKS_PER_THREAD = 5;
  const int TOTAL_TASKS = THREAD_COUNT * TASKS_PER_THREAD;

  std::vector<std::thread> threads;
  std::vector<executor_loop> executors(THREAD_COUNT);

  // Start threads
  threads.reserve(THREAD_COUNT);
  for (int i = 0; i < THREAD_COUNT; i++) {
    threads.emplace_back([&, i]() {
      executor_loop& executor = executors[i];

      for (int j = 0; j < TASKS_PER_THREAD; j++) {
        semaphore_mt_basic_test(sem, counter).detach_with_callback(executor, [&]() {
          if (++completed == TOTAL_TASKS) {
            for (auto& exec : executors) {
              exec.stop();
            }
          }
        });
      }

      executor.run_loop();
    });
  }

  // Wait for threads to finish
  for (auto& t : threads) {
    t.join();
  }

  ASSERT(counter == TOTAL_TASKS);
  LOG("Basic test: OK (counter=%d)", counter.load());
}

void run_resource_pool_test() {
  LOG("=== Multi-threaded Resource Pool Test ===");

  counting_semaphore sem(5);  // Pool of 5 resources
  std::atomic<int> max_concurrent{0};
  std::atomic<int> current_using{0};
  std::atomic<int> completed{0};

  const int THREAD_COUNT = 8;
  const int TASKS_PER_THREAD = 5;
  const int TOTAL_TASKS = THREAD_COUNT * TASKS_PER_THREAD;

  std::vector<std::thread> threads;
  std::vector<executor_loop> executors(THREAD_COUNT);

  // Start threads
  threads.reserve(THREAD_COUNT);
  for (int i = 0; i < THREAD_COUNT; i++) {
    threads.emplace_back([&, i]() {
      executor_loop& executor = executors[i];

      for (int j = 0; j < TASKS_PER_THREAD; j++) {
        int task_id = i * TASKS_PER_THREAD + j;
        semaphore_mt_resource_pool_test(sem, max_concurrent, current_using, task_id).detach_with_callback(executor, [&]() {
          if (++completed == TOTAL_TASKS) {
            for (auto& exec : executors) {
              exec.stop();
            }
          }
        });
      }

      executor.run_loop();
    });
  }

  // Wait for threads to finish
  for (auto& t : threads) {
    t.join();
  }

  ASSERT(max_concurrent <= 5);
  ASSERT(sem.available() == 5);
  LOG("Resource pool test: OK (max_concurrent=%d)", max_concurrent.load());
}

void run_producer_consumer_test() {
  LOG("=== Multi-threaded Producer-Consumer Test ===");

  const int BUFFER_SIZE = 5;
  const int ITEM_COUNT = 20;

  counting_semaphore empty_slots(BUFFER_SIZE);  // Initially all empty
  counting_semaphore filled_slots(0);           // Initially no filled slots

  std::atomic<int> produced{0};
  std::atomic<int> consumed{0};

  const int THREAD_COUNT = 2;
  std::vector<std::thread> threads;
  std::vector<executor_loop> executors(THREAD_COUNT);  // Create executors in main scope

  // Producer thread
  threads.emplace_back([&, idx = 0]() {
    executor_loop& executor = executors[idx];

    producer(empty_slots, filled_slots, produced, ITEM_COUNT).detach_with_callback(executor, [&]() {
      executor.stop();
    });

    executor.run_loop();
  });

  // Consumer thread
  threads.emplace_back([&, idx = 1]() {
    executor_loop& executor = executors[idx];

    consumer(empty_slots, filled_slots, consumed, ITEM_COUNT).detach_with_callback(executor, [&]() {
      executor.stop();
    });

    executor.run_loop();
  });

  for (auto& t : threads) {
    t.join();
  }

  ASSERT(produced == ITEM_COUNT);
  ASSERT(consumed == ITEM_COUNT);

  LOG("Producer-consumer test: OK (produced=%d, consumed=%d)", produced.load(), consumed.load());
}

void run_binary_semaphore_test() {
  LOG("=== Multi-threaded Binary Semaphore Test ===");

  binary_semaphore sem(1);
  std::atomic<int> counter{0};
  std::atomic<int> max_concurrent{0};
  std::atomic<int> completed{0};

  const int THREAD_COUNT = 8;
  const int ITERATIONS = 10;

  std::vector<std::thread> threads;
  std::vector<executor_loop> executors(THREAD_COUNT);

  // Start threads
  threads.reserve(THREAD_COUNT);
  for (int i = 0; i < THREAD_COUNT; i++) {
    threads.emplace_back([&, i]() {
      executor_loop& executor = executors[i];

      semaphore_mt_binary_test(sem, counter, max_concurrent, ITERATIONS).detach_with_callback(executor, [&]() {
        if (++completed == THREAD_COUNT) {
          for (auto& exec : executors) {
            exec.stop();
          }
        }
      });

      executor.run_loop();
    });
  }

  // Wait for threads to finish
  for (auto& t : threads) {
    t.join();
  }

  ASSERT(max_concurrent == 1);
  ASSERT(counter == 0);
  LOG("Binary semaphore test: OK (max_concurrent=%d)", max_concurrent.load());
}

int main() {
  LOG("Semaphore multi-threaded test init");

  run_basic_test();
  run_resource_pool_test();
  run_producer_consumer_test();
  run_binary_semaphore_test();

  LOG("=== All Multi-threaded Semaphore Tests Passed ===");

  check_coro_leak();
  return 0;
}
