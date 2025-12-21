/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include "log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include <atomic>
#include <thread>
#include <vector>

#include "TimeCount.hpp"
#include "assert_def.h"
#include "coro.hpp"
#include "utils.hpp"

using namespace coro;

#ifndef CORO_USE_SINGLE_THREAD
template <typename T>
using channel_t = mt_channel<T>;
#else
template <typename T>
using channel_t = st_channel<T>;
#endif

// Coroutine task that acquires mutex and modifies shared counter
// This task will run on its own executor in a separate thread
async<void> mutex_task(coro::mutex& mtx, std::atomic<int>& shared_counter, int task_id, executor& expected_exec, std::thread::id expected_tid) {
  LOG("Test mutex_task task_id: %d", task_id);
  ASSERT(std::this_thread::get_id() == expected_tid);

  ASSERT(co_await current_executor() == &expected_exec);
  ASSERT(std::this_thread::get_id() == expected_tid);

  for (int i = 0; i < 10; ++i) {
    // Acquire the mutex
    auto guard = co_await mtx.scoped_lock();
    ASSERT(co_await current_executor() == &expected_exec);
    ASSERT(std::this_thread::get_id() == expected_tid);

    // Critical section: read-modify-write on shared counter
    int temp = shared_counter.load();
    LOG("Task %d: iteration %d, got lock, counter = %d", task_id, i, temp);

    // Simulate some work with the lock held
    co_await sleep(5ms);
    ASSERT(co_await current_executor() == &expected_exec);
    ASSERT(std::this_thread::get_id() == expected_tid);

    // Increment the counter
    shared_counter.store(temp + 1);
    LOG("Task %d: iteration %d, incremented counter to %d", task_id, i, shared_counter.load());

    // Lock is automatically released when guard goes out of scope
  }

  ASSERT(co_await current_executor() == &expected_exec);
  ASSERT(std::this_thread::get_id() == expected_tid);
  LOG("Task %d: finished", task_id);
}

async<void> run_mutex_test(executor& exec1, executor& exec2, std::thread::id exec1_tid, std::thread::id exec2_tid) {
  LOG("=== Testing Mutex Across Threads ===");

  // Create a shared mutex and counter
  coro::mutex mtx;
  std::atomic<int> shared_counter{0};
  TimeCount t;

  // Launch two tasks on different executors (different threads)
  auto task1 = mutex_task(mtx, shared_counter, 1, exec1, exec1_tid).bind_executor(exec1);
  auto task2 = mutex_task(mtx, shared_counter, 2, exec2, exec2_tid).bind_executor(exec2);

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

// Channel producer task
async<void> channel_producer(channel_t<int>& ch, int producer_id, int num_items, executor& expected_exec, std::thread::id expected_tid) {
  LOG("Producer %d: started, will send %d items", producer_id, num_items);
  ASSERT(std::this_thread::get_id() == expected_tid);

  ASSERT(co_await current_executor() == &expected_exec);
  ASSERT(std::this_thread::get_id() == expected_tid);

  for (int i = 0; i < num_items; ++i) {
    int value = producer_id * 1000 + i;
    LOG("Producer %d: sending value %d", producer_id, value);

    bool success = co_await ch.send(value);
    ASSERT(co_await current_executor() == &expected_exec);
    ASSERT(std::this_thread::get_id() == expected_tid);

    if (!success) {
      LOG("Producer %d: channel closed, stopping", producer_id);
      break;
    }

    LOG("Producer %d: sent value %d", producer_id, value);
    co_await sleep(10ms);  // Simulate some work
    ASSERT(co_await current_executor() == &expected_exec);
    ASSERT(std::this_thread::get_id() == expected_tid);
  }

  LOG("Producer %d: finished", producer_id);
}

// Channel consumer task
async<void> channel_consumer(channel_t<int>& ch, int consumer_id, std::vector<int>& received_values, int expected_count, executor& expected_exec,
                             std::thread::id expected_tid) {
  LOG("Consumer %d: started", consumer_id);

  ASSERT(co_await current_executor() == &expected_exec);
  ASSERT(std::this_thread::get_id() == expected_tid);

  while (true) {
    LOG("Consumer %d: waiting for value", consumer_id);
    auto value = co_await ch.recv();
    ASSERT(co_await current_executor() == &expected_exec);
    ASSERT(std::this_thread::get_id() == expected_tid);

    if (!value.has_value()) {
      LOG("Consumer %d: channel closed, stopping", consumer_id);
      break;
    }

    LOG("Consumer %d: received value %d", consumer_id, value.value());
    received_values.push_back(value.value());

    co_await sleep(15ms);  // Simulate some work
    ASSERT(co_await current_executor() == &expected_exec);
    ASSERT(std::this_thread::get_id() == expected_tid);

    // If we know the expected count and have received that many items, we can break early
    if (expected_count > 0 && received_values.size() >= static_cast<size_t>(expected_count)) {
      LOG("Consumer %d: received expected count (%d), stopping", consumer_id, expected_count);
      break;
    }
  }

  LOG("Consumer %d: finished, received %zu items", consumer_id, received_values.size());
}

async<void> run_channel_test(executor& exec1, executor& exec2, std::thread::id exec1_tid, std::thread::id exec2_tid) {
  LOG("=== Testing Channel Across Threads ===");

  // Test 1: Unbuffered channel (capacity = 0)
  {
    LOG("--- Test 1: Unbuffered Channel ---");
    channel_t<int> ch(0);  // Unbuffered
    std::vector<int> received1, received2;
    TimeCount t;

    // Create tasks
    auto producer1 = channel_producer(ch, 1, 5, exec1, exec1_tid).bind_executor(exec1);
    auto producer2 = channel_producer(ch, 2, 5, exec2, exec2_tid).bind_executor(exec2);
    auto consumer1 = channel_consumer(ch, 1, received1, 5, exec1, exec1_tid).bind_executor(exec1);
    auto consumer2 = channel_consumer(ch, 2, received2, 5, exec2, exec2_tid).bind_executor(exec2);

    // Run all tasks concurrently
    co_await when_all(std::move(producer1), std::move(producer2), std::move(consumer1), std::move(consumer2));

    ch.close();

    int total_received = received1.size() + received2.size();
    LOG("Unbuffered channel test: received %d items (expected 10), elapsed: %d ms", total_received, (int)t.elapsed());
    ASSERT(total_received == 10);
    LOG("--- Unbuffered Channel Test PASSED ---");
  }

  // Test 2: Buffered channel (capacity = 3)
  {
    LOG("--- Test 2: Buffered Channel (capacity=3) ---");
    channel_t<int> ch(3);  // Buffered with capacity 3
    std::vector<int> received;
    TimeCount t;

    auto producer = channel_producer(ch, 3, 10, exec1, exec1_tid)
                        .with_callback([] {
                          LOG("producer end");
                        })
                        .bind_executor(exec1);
    auto consumer = channel_consumer(ch, 3, received, 10, exec2, exec2_tid)
                        .with_callback([] {
                          LOG("consumer end");
                        })
                        .bind_executor(exec2);

    co_await when_all(std::move(producer), std::move(consumer));

    ch.close();

    LOG("Buffered channel test: received %zu items (expected 10), elapsed: %d ms", received.size(), (int)t.elapsed());
    ASSERT(received.size() == 10);

    // Verify all values were received in order
    for (size_t i = 0; i < received.size(); ++i) {
      int expected = 3000 + i;
      ASSERT(received[i] == expected);
    }
    LOG("--- Buffered Channel Test PASSED ---");
  }

  // Test 3: Multiple producers, single consumer
  {
    LOG("--- Test 3: Multiple Producers, Single Consumer ---");
    channel_t<int> ch(5);
    std::vector<int> received;
    TimeCount t;

    auto producer1 = channel_producer(ch, 4, 8, exec1, exec1_tid).bind_executor(exec1);
    auto producer2 = channel_producer(ch, 5, 8, exec2, exec2_tid).bind_executor(exec2);
    auto producer3 = channel_producer(ch, 6, 8, exec1, exec1_tid).bind_executor(exec1);
    auto consumer = channel_consumer(ch, 4, received, 24, exec2, exec2_tid).bind_executor(exec2);

    co_await when_all(std::move(producer1), std::move(producer2), std::move(producer3), std::move(consumer));

    ch.close();

    LOG("Multi-producer test: received %zu items (expected 24), elapsed: %d ms", received.size(), (int)t.elapsed());
    ASSERT(received.size() == 24);
    LOG("--- Multiple Producers Test PASSED ---");
  }

  LOG("=== Channel Multi-Thread Test PASSED ===");
}

int main() {
  LOG("Multi-thread test init");

#ifndef CORO_USE_SINGLE_THREAD
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

  // Thread 3: Run executor main
  std::thread thread3([&] {
    exec3_tid = std::this_thread::get_id();
    exec_tid_ready.fetch_add(1, std::memory_order_release);
    LOG("Thread 3: starting executor loop");
    exec3.run_loop();
    LOG("Thread 3: executor loop stopped");
  });

  while (exec_tid_ready.load(std::memory_order_acquire) < 3) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
#else
  executor_loop exec1;
  executor_loop& exec2 = exec1;
  executor_loop& exec3 = exec1;

  // Record thread IDs
  std::thread::id exec1_tid;
  std::thread::id& exec2_tid = exec1_tid;
  exec1_tid = std::this_thread::get_id();
  LOG("Thread 1: starting executor loop");
  LOG("Thread 1: executor loop stopped");
#endif

  // Start the tests on executor 1
  auto test_coro = [](auto& exec1, auto& exec2, auto exec1_tid, auto exec2_tid) -> async<void> {
    // Run mutex test
    co_await run_mutex_test(exec1, exec2, exec1_tid, exec2_tid);

    // Run channel test
    co_await run_channel_test(exec1, exec2, exec1_tid, exec2_tid);

    LOG("All tests completed");
  }(exec1, exec2, exec1_tid, exec2_tid);

  test_coro.detach_with_callback(exec3, [&] {
    LOG("Test completed, stopping executors");
    exec1.stop();
    exec2.stop();
    exec3.stop();
  });

  // Wait for both executors to finish

#ifndef CORO_USE_SINGLE_THREAD
  thread1.join();
  thread2.join();
  thread3.join();
#else
  exec1.run_loop();
#endif
  LOG("Test completed successfully");

  check_coro_leak();
  return 0;
}
