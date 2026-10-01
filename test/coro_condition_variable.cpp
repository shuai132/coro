/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include "log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "assert_def.h"
#include "coro.hpp"
#include "utils.hpp"

using namespace coro;

#ifndef CORO_DISABLE_EXCEPTION
struct failing_unlock_mutex {
  std::function<void()> before_failure_;
  bool fail_ = true;

  void unlock() {
    if (std::exchange(fail_, false)) {
      if (before_failure_) {
        before_failure_();
      }
      throw std::runtime_error("user unlock failed");
    }
  }
};

template <typename CV>
async<void> await_unlock_failure(CV& cv, failing_unlock_mutex& mutex, bool& caught) {
  try {
    co_await cv.wait(mutex);
  } catch (const std::runtime_error&) {
    caught = true;
  }
}

template <typename CV>
async<void> await_after_unlock_failure(CV& cv, bool& completed) {
  dummy_mutex mutex;
  co_await cv.wait(mutex);
  completed = true;
}

template <typename CV>
void test_unlock_failure_cleanup() {
  for (bool notify_during_unlock : {false, true}) {
    CV cv;
    failing_unlock_mutex mutex;
    if (notify_during_unlock) {
      mutex.before_failure_ = [&] {
        cv.notify_all();
      };
    }
    bool caught = false;
    {
      auto failed = await_unlock_failure(cv, mutex, caught);
      failed.current_coro_handle_.resume();
      ASSERT(failed.current_coro_handle_.done());
      failed.await_resume();
    }
    ASSERT(caught);
    // Exercise notification after the failed frame has been destroyed.
    cv.notify_one();
    cv.notify_all();
    bool completed = false;
    {
      auto next = await_after_unlock_failure(cv, completed);
      next.current_coro_handle_.resume();
      ASSERT(!completed);
      cv.notify_one();
      ASSERT(completed);
      next.await_resume();
    }
    check_coro_leak();
  }
}

struct unlock_failure_handoff_state {
  std::atomic<bool> notifier_dequeued_{false};
  std::atomic<bool> release_notifier_{false};
  std::atomic<bool> armed_{false};
};

static unlock_failure_handoff_state* current_unlock_failure_handoff_state;

struct paused_notification_mutex {
  std::mutex mutex_;
  unlock_failure_handoff_state* state_ = current_unlock_failure_handoff_state;

  void lock() {
    mutex_.lock();
  }
  void unlock() noexcept {
    mutex_.unlock();
    if (state_->armed_.exchange(false, std::memory_order_acq_rel)) {
      state_->notifier_dequeued_.store(true, std::memory_order_release);
      while (!state_->release_notifier_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
  }
};

void test_unlock_failure_during_notification_handoff() {
  unlock_failure_handoff_state state;
  current_unlock_failure_handoff_state = &state;
  condition_variable_t<paused_notification_mutex> cv;
  std::thread notifier;
  failing_unlock_mutex mutex;
  mutex.before_failure_ = [&] {
    state.armed_.store(true, std::memory_order_release);
    notifier = std::thread([&] {
      cv.notify_one();
    });
    while (!state.notifier_dequeued_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  };
  bool caught = false;
  {
    auto task = await_unlock_failure(cv, mutex, caught);
    task.current_coro_handle_.resume();
    // The notifier owns the node, so the frame must stay alive until it finishes.
    ASSERT(!task.current_coro_handle_.done());
    ASSERT(!caught);
    state.release_notifier_.store(true, std::memory_order_release);
    notifier.join();
    ASSERT(task.current_coro_handle_.done());
    ASSERT(caught);
    task.await_resume();
  }
  current_unlock_failure_handoff_state = nullptr;
  check_coro_leak();
}

template <typename CV>
void test_unlock_failure_preserves_other_waiters() {
  for (int failed_index : {0, 1, 2}) {
    CV cv;
    bool completed[2]{};
    auto first = await_after_unlock_failure(cv, completed[0]);
    auto second = await_after_unlock_failure(cv, completed[1]);
    auto enqueue = [&] {
      if (!completed[0] && !first.current_coro_handle_.promise().started_) {
        first.current_coro_handle_.resume();
      } else {
        second.current_coro_handle_.resume();
      }
    };
    failing_unlock_mutex mutex;
    for (int i = 0; i < failed_index; ++i) {
      enqueue();
    }
    mutex.before_failure_ = [&] {
      for (int i = failed_index; i < 2; ++i) {
        enqueue();
      }
    };
    bool caught = false;
    {
      auto failed = await_unlock_failure(cv, mutex, caught);
      failed.current_coro_handle_.resume();
      ASSERT(failed.current_coro_handle_.done());
      ASSERT(caught);
      failed.await_resume();
    }
    cv.notify_one();
    ASSERT(completed[0]);
    ASSERT(!completed[1]);
    cv.notify_all();
    ASSERT(completed[1]);
    first.await_resume();
    second.await_resume();
  }
  check_coro_leak();
}
#endif

struct ready_awaitable {
  bool await_ready() noexcept {
    return true;
  }

  void await_suspend(std::coroutine_handle<>) noexcept {}

  void await_resume() noexcept {}
};

struct notify_on_unlock_mutex {
  coro::condition_variable* cv{};
  bool unlocked{};

  void unlock() {
    unlocked = true;
    cv->notify_one();
  }

  ready_awaitable lock() noexcept {
    return {};
  }
};

struct internal_unlock_state {
  std::unique_ptr<async<void>> owner;
  std::function<void()> notify;
  int unlock_count = 0;
  int trigger_unlock = 0;
  bool mutex_destroyed = false;
  bool completed = false;
};

static internal_unlock_state* current_internal_unlock_state;

struct observed_internal_mutex {
  std::mutex mutex_;
  internal_unlock_state* state_ = current_internal_unlock_state;

  ~observed_internal_mutex() {
    state_->mutex_destroyed = true;
  }
  void lock() {
    mutex_.lock();
  }
  void unlock() {
    auto* state = state_;
    mutex_.unlock();
    if (++state->unlock_count == state->trigger_unlock) {
      state->owner.reset();
      state->notify();
      // The unlock implementation is still using its mutex object until it returns.
      ASSERT(!state->mutex_destroyed);
    }
  }
};

async<void> wait_for_internal_unlock(internal_unlock_state& state, bool notify_all) {
  condition_variable_t<observed_internal_mutex> cv;
  dummy_mutex user_mutex;
  state.notify = [&cv, notify_all] {
    if (notify_all) {
      cv.notify_all();
    } else {
      cv.notify_one();
    }
  };
  user_mutex.lock();
  co_await cv.wait(user_mutex);
  state.completed = true;
}

void test_notification_from_internal_unlock() {
  for (bool notify_all : {false, true}) {
    for (int trigger_unlock : {1, 2}) {
      internal_unlock_state state;
      state.trigger_unlock = trigger_unlock;
      current_internal_unlock_state = &state;
      state.owner = std::make_unique<async<void>>(wait_for_internal_unlock(state, notify_all));
      state.owner->current_coro_handle_.resume();
      if (!state.completed) {
        state.notify();
      }
      ASSERT(state.completed);
      ASSERT(state.mutex_destroyed);
      ASSERT(!state.owner);
      current_internal_unlock_state = nullptr;
      check_coro_leak();
    }
  }
}

template <typename CV>
async<void> cv_reused_waiter_test() {
  CV cv;
  coro::mutex mutex;
  auto waiter = cv.wait(mutex);
  auto copied = waiter;
  auto moved = std::move(copied);
  moved = waiter;
  auto* executor = co_await current_executor();
  for (int iteration = 0; iteration < 32; ++iteration) {
    co_await mutex.lock();
    executor->post([&cv, iteration] {
      if (iteration % 2) {
        cv.notify_all();
      } else {
        cv.notify_one();
      }
    });
    co_await moved;
    ASSERT(!mutex.is_locked());
  }
}

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

// Test notify racing with wait's unlock path
async<void> cv_notify_during_unlock_test() {
  coro::condition_variable cv;
  notify_on_unlock_mutex mtx{&cv};
  bool resumed = false;

  co_await cv.wait(mtx);
  resumed = true;

  ASSERT(mtx.unlocked);
  ASSERT(resumed);
  LOG("Notify during unlock test passed: OK");
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

  co_await spawn_local(producer(1));
  co_await spawn_local(producer(2));

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
  co_await cv_reused_waiter_test<condition_variable_mt>();
  co_await cv_reused_waiter_test<condition_variable_st>();
  {
    co_await cv_basic_test();
    LOG("Basic test completed\n");
  }

  {
    co_await cv_notify_during_unlock_test();
    LOG("Notify during unlock test completed\n");
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
#ifndef CORO_DISABLE_EXCEPTION
  test_unlock_failure_cleanup<condition_variable_mt>();
  test_unlock_failure_cleanup<condition_variable_st>();
  test_unlock_failure_preserves_other_waiters<condition_variable_mt>();
  test_unlock_failure_preserves_other_waiters<condition_variable_st>();
  test_unlock_failure_during_notification_handoff();
#endif
  test_notification_from_internal_unlock();
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
