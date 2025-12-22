/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include "log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include "assert_def.h"
#include "coro.hpp"
#include "utils.hpp"

using namespace coro;

// Test basic notify_one functionality
async<void> cv_basic_test() {
  coro::mutex mtx;
  coro::condition_variable cv;
  bool ready = false;

  auto waiter = [&mtx, &cv, &ready]() -> async<void> {
    LOG("Waiter: acquiring lock");
    co_await mtx.lock();
    LOG("Waiter: acquired lock, waiting for condition");

    while (!ready) {
      co_await cv.wait(mtx);
      co_await mtx.lock();  // Re-acquire lock after wait
      LOG("Waiter: woke up, ready=%d", ready);
    }

    LOG("Waiter: condition met, releasing lock");
    mtx.unlock();
  };

  auto notifier = [&mtx, &cv, &ready]() -> async<void> {
    co_await sleep(50ms);
    LOG("Notifier: acquiring lock");
    co_await mtx.lock();
    LOG("Notifier: setting ready=true");
    ready = true;
    LOG("Notifier: releasing lock");
    mtx.unlock();
    LOG("Notifier: notifying one");
    cv.notify_one();
  };

  co_await when_all(waiter(), notifier());
  ASSERT(ready);
  LOG("Basic test passed: OK");
}

// Test notify_all functionality
async<void> cv_notify_all_test() {
  coro::mutex mtx;
  coro::condition_variable cv;
  int ready_count = 0;
  int waiter_count = 0;

  auto waiter = [&mtx, &cv, &ready_count, &waiter_count](int id) -> async<void> {
    LOG("Waiter %d: acquiring lock", id);
    co_await mtx.lock();
    waiter_count++;
    LOG("Waiter %d: waiting for condition", id);

    co_await cv.wait(mtx);
    co_await mtx.lock();  // Re-acquire lock after wait

    LOG("Waiter %d: woke up", id);
    ready_count++;
    mtx.unlock();
    LOG("Waiter %d: done", id);
  };

  auto notifier = [&mtx, &cv, &waiter_count]() -> async<void> {
    // Wait for all waiters to be ready
    while (true) {
      co_await mtx.lock();
      if (waiter_count >= 3) {
        mtx.unlock();
        break;
      }
      mtx.unlock();
      co_await sleep(10ms);
    }

    co_await sleep(50ms);
    LOG("Notifier: notifying all");
    cv.notify_all();
  };

  co_await when_all(waiter(1), waiter(2), waiter(3), notifier());
  ASSERT(ready_count == 3);
  LOG("Notify all test passed: OK");
}

// Test producer-consumer pattern
async<void> cv_producer_consumer_test() {
  coro::mutex mtx;
  coro::condition_variable cv;
  std::vector<int> queue;
  const int max_size = 5;
  bool done = false;

  auto producer = [&mtx, &cv, &queue, &done](int id) -> async<void> {
    for (int i = 0; i < 3; i++) {
      co_await mtx.lock();

      // Wait until queue has space
      while (queue.size() >= max_size) {
        LOG("Producer %d: queue full, waiting", id);
        co_await cv.wait(mtx);
        co_await mtx.lock();
      }

      int value = id * 10 + i;
      queue.push_back(value);
      LOG("Producer %d: produced %d, queue size: %zu", id, value, queue.size());
      mtx.unlock();
      cv.notify_all();  // Notify consumers and other producers

      co_await sleep(10ms);
    }

    co_await mtx.lock();
    done = true;
    mtx.unlock();
    cv.notify_all();
    LOG("Producer %d: finished", id);
  };

  auto consumer = [&mtx, &cv, &queue, &done](int id) -> async<int> {
    int consumed = 0;
    while (true) {
      co_await mtx.lock();

      // Wait until queue has data or done
      while (queue.empty() && !done) {
        LOG("Consumer %d: queue empty, waiting", id);
        co_await cv.wait(mtx);
        co_await mtx.lock();
      }

      if (!queue.empty()) {
        int value = queue.front();
        queue.erase(queue.begin());
        consumed++;
        LOG("Consumer %d: consumed %d, queue size: %zu", id, value, queue.size());
        mtx.unlock();
        cv.notify_all();  // Notify producers and other consumers
        co_await sleep(15ms);
      } else {
        mtx.unlock();
        break;
      }
    }
    LOG("Consumer %d: finished, consumed %d items", id, consumed);
    co_return consumed;
  };

  auto exec = co_await current_executor();
  co_spawn(*exec, producer(1));
  co_spawn(*exec, producer(2));

  auto c1 = consumer(1);
  auto c2 = consumer(2);

  auto [total1, total2] = co_await when_all(std::move(c1), std::move(c2));

  ASSERT(total1 + total2 == 6);  // 2 producers * 3 items each
  ASSERT(queue.empty());
  LOG("Producer-consumer test passed: OK (consumed: %d + %d = %d)", total1, total2, total1 + total2);
}

// Test multiple waiters with notify_one
async<void> cv_multiple_waiters_test() {
  coro::mutex mtx;
  coro::condition_variable cv;
  int wakeup_count = 0;

  auto waiter = [&mtx, &cv, &wakeup_count](int id) -> async<void> {
    co_await mtx.lock();
    LOG("Waiter %d: waiting", id);
    co_await cv.wait(mtx);
    co_await mtx.lock();
    wakeup_count++;
    LOG("Waiter %d: woke up, wakeup_count=%d", id, wakeup_count);
    mtx.unlock();
  };

  auto notifier = [&cv]() -> async<void> {
    for (int i = 0; i < 3; i++) {
      co_await sleep(50ms);
      LOG("Notifier: notify_one #%d", i + 1);
      cv.notify_one();
    }
  };

  co_await when_all(waiter(1), waiter(2), waiter(3), notifier());
  ASSERT(wakeup_count == 3);
  LOG("Multiple waiters test passed: OK");
}

// Test spurious wakeup handling (wait in loop with condition check)
async<void> cv_spurious_wakeup_test() {
  coro::mutex mtx;
  coro::condition_variable cv;
  int counter = 0;
  bool ready = false;

  auto waiter = [&mtx, &cv, &ready, &counter]() -> async<void> {
    co_await mtx.lock();
    LOG("Waiter: waiting for ready");

    // Standard pattern: wait in a loop with condition check
    while (!ready) {
      counter++;
      co_await cv.wait(mtx);
      co_await mtx.lock();
      LOG("Waiter: woke up, ready=%d, counter=%d", ready, counter);
    }

    LOG("Waiter: condition met");
    mtx.unlock();
  };

  auto notifier = [&mtx, &cv, &ready]() -> async<void> {
    // First notify without setting ready (spurious wakeup simulation)
    co_await sleep(30ms);
    LOG("Notifier: first notify (without setting ready)");
    cv.notify_one();

    // Then notify with ready set
    co_await sleep(50ms);
    co_await mtx.lock();
    ready = true;
    mtx.unlock();
    LOG("Notifier: second notify (with ready=true)");
    cv.notify_one();
  };

  co_await when_all(waiter(), notifier());
  ASSERT(ready);
  ASSERT(counter >= 2);  // Should wake up at least twice
  LOG("Spurious wakeup test passed: OK (counter=%d)", counter);
}

// Test wait with predicate (simplified syntax)
async<void> cv_wait_with_predicate_test() {
  coro::mutex mtx;
  coro::condition_variable cv;
  bool ready = false;

  auto waiter = [&mtx, &cv, &ready]() -> async<void> {
    LOG("Waiter: acquiring lock");
    co_await mtx.lock();
    LOG("Waiter: waiting with predicate");

    // Simplified syntax - no manual loop needed
    co_await cv.wait(mtx, [&] {
      return ready;
    });

    LOG("Waiter: predicate is true, lock is held");
    ASSERT(ready);
    mtx.unlock();
  };

  auto notifier = [&mtx, &cv, &ready]() -> async<void> {
    // First notify without setting ready (should not wake up waiter)
    co_await sleep(30ms);
    LOG("Notifier: first notify (without setting ready)");
    cv.notify_one();

    // Then notify with ready set
    co_await sleep(50ms);
    co_await mtx.lock();
    ready = true;
    LOG("Notifier: setting ready=true");
    mtx.unlock();
    LOG("Notifier: notifying with ready=true");
    cv.notify_one();
  };

  co_await when_all(waiter(), notifier());
  ASSERT(ready);
  LOG("Wait with predicate test passed: OK");
}

// Test wait with predicate - complex condition
async<void> cv_wait_predicate_complex_test() {
  coro::mutex mtx;
  coro::condition_variable cv;
  int counter = 0;
  const int target = 5;

  auto incrementer = [&mtx, &cv, &counter](int id) -> async<void> {
    for (int i = 0; i < 2; i++) {
      co_await sleep(20ms * id);
      co_await mtx.lock();
      counter++;
      LOG("Incrementer %d: counter=%d", id, counter);
      mtx.unlock();
      cv.notify_all();
    }
  };

  auto waiter = [&mtx, &cv, &counter]() -> async<void> {
    co_await mtx.lock();
    LOG("Waiter: waiting for counter >= %d", target);

    // Wait with complex predicate
    co_await cv.wait(mtx, [&] {
      LOG("Waiter: checking predicate, counter=%d", counter);
      return counter >= target;
    });

    LOG("Waiter: condition met, counter=%d", counter);
    ASSERT(counter >= target);
    mtx.unlock();
  };

  co_await when_all(incrementer(1), incrementer(2), incrementer(3), waiter());
  ASSERT(counter >= target);
  LOG("Wait with complex predicate test passed: OK (counter=%d)", counter);
}

async<void> run_all_tests() {
  {
    co_await cv_basic_test();
    LOG("Basic test completed\n");
  }

  {
    co_await cv_notify_all_test();
    LOG("Notify all test completed\n");
  }

  {
    co_await cv_multiple_waiters_test();
    LOG("Multiple waiters test completed\n");
  }

  {
    co_await cv_spurious_wakeup_test();
    LOG("Spurious wakeup test completed\n");
  }

  {
    co_await cv_wait_with_predicate_test();
    LOG("Wait with predicate test completed\n");
  }

  {
    co_await cv_wait_predicate_complex_test();
    LOG("Wait with complex predicate test completed\n");
  }

  {
    co_await cv_producer_consumer_test();
    LOG("Producer-consumer test completed\n");
  }
}

int main() {
  LOG("Condition Variable test init");
  executor_loop executor;
  run_all_tests().detach_with_callback(executor, [&] {
    executor.stop();
  });
  LOG("loop...");
  executor.run_loop();
  check_coro_leak();
  return 0;
}
