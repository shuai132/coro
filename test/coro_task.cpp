/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include "log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include <array>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "TimeCount.hpp"
#include "assert_def.h"
#include "coro/coro.hpp"
#include "coro/time.hpp"
#include "noinline.hpp"
#include "utils.hpp"

/// config executor
#define CORO_EXECUTOR_LOOP
#define CORO_EXECUTOR_POLL

#ifdef CORO_EXECUTOR_LOOP
#include "coro/executor_loop.hpp"
#elif defined(CORO_EXECUTOR_POLL)
#include "coro/executor_poll.hpp"
#endif

using namespace coro;

#ifndef CORO_DISABLE_EXCEPTION
// Fault injection is confined to the thread performing the timer insertion.
thread_local bool fail_next_task_allocation = false;
thread_local int task_allocations_before_failure = 0;

CORO_TEST_NOINLINE void* operator new(std::size_t size) {
  if (fail_next_task_allocation) {
    if (task_allocations_before_failure == 0) {
      fail_next_task_allocation = false;
      throw std::bad_alloc();
    }
    --task_allocations_before_failure;
  }
  if (auto* memory = std::malloc(size ? size : 1)) {
    return memory;
  }
  throw std::bad_alloc();
}

CORO_TEST_NOINLINE void operator delete(void* memory) noexcept {
  std::free(memory);
}

CORO_TEST_NOINLINE void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}

struct post_on_timer_cleanup {
  executor& exec_;
  bool& called_;

  post_on_timer_cleanup(executor& exec, bool& called) : exec_(exec), called_(called) {}

  ~post_on_timer_cleanup() {
    exec_.post([&called = called_] {
      called = true;
    });
  }
};

template <typename EXECUTOR>
void test_failed_timer_cleanup_reenters_executor(int allocations_before_failure = 0) {
  EXECUTOR exec;
  bool cleanup_called = false;
  auto resource = std::make_shared<post_on_timer_cleanup>(exec, cleanup_called);
  // Force heap storage: some std::function implementations retain small targets after a move.
  std::function<void()> callback = [resource = std::move(resource), padding = std::array<char, 128>{}] {
    (void)padding;
    ASSERT(false);
  };
  bool allocation_failed = false;
  std::atomic<bool> finished{false};
  std::thread worker([&] {
    task_allocations_before_failure = allocations_before_failure;
    fail_next_task_allocation = true;
    try {
      exec.post_delayed_ns(std::move(callback), 0);
    } catch (const std::bad_alloc&) {
      allocation_failed = true;
    }
    finished.store(true, std::memory_order_release);
  });
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT(finished.load(std::memory_order_acquire));
  worker.join();
  ASSERT(allocation_failed);
  if constexpr (std::is_same_v<EXECUTOR, executor_poll>) {
    exec.poll();
  } else {
    exec.post([&exec] {
      exec.stop();
    });
    exec.run_loop();
  }
  ASSERT(cleanup_called);
}
#endif

struct reentrant_task_state {
  executor* exec_;
  std::atomic<bool> armed_{false};
  bool reenter_on_copy_;
  bool called_ = false;
  int cleanup_posts_ = 0;
};

struct reentrant_task_target {
  reentrant_task_state* state_;

  explicit reentrant_task_target(reentrant_task_state& state) noexcept : state_(&state) {}
  reentrant_task_target(const reentrant_task_target& other) noexcept : state_(other.state_) {
    if (state_->armed_.load(std::memory_order_acquire) && state_->reenter_on_copy_) {
      post_cleanup();
    }
  }
  ~reentrant_task_target() {
    if (state_->armed_.load(std::memory_order_acquire) && !state_->reenter_on_copy_) {
      post_cleanup();
    }
  }
  void operator()() const {
    state_->called_ = true;
  }

 private:
  void post_cleanup() const {
    state_->exec_->post([state = state_] {
      ++state->cleanup_posts_;
    });
  }
};

enum class queued_task_kind { posted, timed, continuation };

void test_loop_destruction_after_published_task(queued_task_kind kind) {
  for (int i = 0; i < 32; ++i) {
    auto loop = std::make_unique<executor_loop>();
    auto* exec = loop.get();
    std::thread producer([exec, kind] {
      auto finish = [exec] {
        exec->stop();
      };
      switch (kind) {
        case queued_task_kind::posted:
          exec->post(std::move(finish));
          break;
        case queued_task_kind::timed:
          exec->post_delayed_ns(std::move(finish), 0);
          break;
        case queued_task_kind::continuation:
          exec->post_continuation(std::move(finish));
          break;
      }
    });
    loop->run_loop();
    // Publishing the final task must finish notification before the loop can consume it and return.
    loop.reset();
    producer.join();
  }
}

template <typename EXECUTOR>
void test_queued_callback_reenters_executor(queued_task_kind kind, bool reenter_on_copy) {
  EXECUTOR exec;
  reentrant_task_state state{&exec, false, reenter_on_copy};
  std::function<void()> callback{reentrant_task_target{state}};
  // A move can copy the small target even during insertion, before its first invocation.
  state.armed_.store(reenter_on_copy, std::memory_order_release);
  std::atomic<bool> finished{false};
  std::thread worker([&] {
    switch (kind) {
      case queued_task_kind::posted:
        exec.post(std::move(callback));
        break;
      case queued_task_kind::timed:
        exec.post_delayed_ns(std::move(callback), 0);
        break;
      case queued_task_kind::continuation:
        exec.post_continuation(std::move(callback));
        break;
    }
    if constexpr (std::is_same_v<EXECUTOR, executor_loop>) {
      exec.post([&exec] {
        exec.stop();
      });
    }
    // libc++ can retain a small target in the moved-from std::function.
    state.armed_.store(true, std::memory_order_release);
    if constexpr (std::is_same_v<EXECUTOR, executor_loop>) {
      exec.run_loop();
    } else {
      for (int i = 0; i < 16; ++i) {
        exec.poll();
      }
    }
    finished.store(true, std::memory_order_release);
  });
  auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT(finished.load(std::memory_order_acquire));
  worker.join();
  state.armed_.store(false, std::memory_order_release);
  ASSERT(state.called_);
  if (!reenter_on_copy) {
    ASSERT(state.cleanup_posts_ > 0);
  }
}

async<int> coro_fun() {
  {
    LOG("sleep 500ms begin");
    TimeCount t;
    co_await sleep(500ms);
    co_await 0ms;
    co_await std::chrono::milliseconds(0);
    ASSERT(int(t.elapsed()) >= 500);
    LOG("sleep 500ms end: %d", (int)t.elapsed());
  }

  LOG("callback_awaiter begin");
  auto ret = co_await callback_awaiter<int>([](auto callback) {
    std::thread([callback = std::move(callback)] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      callback(123);
    }).detach();
  });
  ASSERT(ret == 123);
  LOG("callback_awaiter end");
  co_return ret;
}

async<void> coro_task() {
  int ret = co_await coro_fun();
  LOG("coro_fun ret: %d", ret);
  ASSERT(ret == 123);
}

async<int> coro_task_int() {
  LOG("coro_task_int 1");
  co_return 1;
}

async<int> coro_task_exception([[maybe_unused]] bool rethrow = false) {
#ifndef CORO_DISABLE_EXCEPTION
  bool flag = false;
  try {
    auto v = co_await []() -> async<int> {
      throw std::runtime_error("coro_task_exception");
      co_return 1;
    }();
    co_return v;
  } catch (std::exception& e) {
    LOG("exception: %s", e.what());
    if (rethrow) {
      std::rethrow_exception(std::current_exception());
    }
    flag = true;
  }
  ASSERT(flag);
#endif

  co_return co_await []() -> async<int> {
    co_return 2;
  }();
}

async<void> loop_task(const char* tag, int ms) {
  int count = 3;
  while (count--) {
    TimeCount t;
    co_await std::chrono::milliseconds(ms);
    LOG("%s: %d, elapsed: %d", tag, ms, (int)t.elapsed());
    ASSERT(int(t.elapsed()) >= ms);
  }
}

void test_coro(executor& executor) {
  // test: loop_task
  spawn(executor, loop_task("A", 100));
  spawn(executor, loop_task("B", 200));
  spawn(executor, loop_task("C", 300));

  /// test: coro_task
  spawn(executor, coro_task());
  // or:
  coro_task().detach(executor);

  /// test: coro_task_int
  coro_task_int().detach(executor);
  // or
  coro_task().detach_with_callback(executor, [&] {
    LOG("coro_task finished");
  });
  // or:
  coro_task_int().with_callback([](int) {}).detach(executor);

  /// test: coro_task_exception
  coro_task_exception().detach_with_callback(executor, [&](int v) {
    LOG("coro_task_exception finished: %d", v);
    ASSERT(v == 2);
  });
  // or:
#ifndef CORO_DISABLE_EXCEPTION
  coro_task_exception(true).detach_with_callback(
      executor,
      [&](int) {
        ASSERT(false);
      },
      [](const std::exception_ptr& e) {
        try {
          std::rethrow_exception(e);
        } catch (const std::runtime_error& e) {
          LOG("coro_task_exception(true) exception: %s", e.what());
          ASSERT(std::string_view(e.what()) == "coro_task_exception");
        }
      });
#endif
}

void test_simple(executor& executor) {
  spawn(executor, [](auto e) -> async<void> {
    ASSERT(e == co_await current_executor());
    TimeCount t;
    const auto ms = 100;
    co_await sleep(std::chrono::milliseconds(ms));
    LOG("%s: %d, elapsed: %d", "test_simple", ms, (int)t.elapsed());
    ASSERT(int(t.elapsed()) >= ms);
  }(&executor));
}

void test_delayed_post_wakes_loop(bool existing_timer) {
  executor_loop executor;
  std::promise<void> started;
  std::promise<void> completed;
  auto completion = completed.get_future();
  if (existing_timer) {
    executor.post_delayed_ns([] {}, 1000000000);
  }
  executor.post([&] {
    started.set_value();
  });
  std::thread worker([&] {
    executor.run_loop();
  });
  started.get_future().wait();
  // Allow the loop to enter its empty-queue or existing-deadline wait.
  std::this_thread::sleep_for(20ms);
  executor.post_delayed_ns(
      [&] {
        completed.set_value();
      },
      10000000);
  bool completed_on_time = completion.wait_for(500ms) == std::future_status::ready;
  executor.stop();
  worker.join();
  ASSERT(completed_on_time);
}

void test_large_delayed_post() {
  for (uint64_t delay : {std::numeric_limits<uint64_t>::max(), static_cast<uint64_t>(std::chrono::nanoseconds::max().count())}) {
    executor_poll executor;
    bool called = false;
    executor.post_delayed_ns(
        [&called] {
          called = true;
        },
        delay);
    executor.poll();
    ASSERT(!called);
  }
}

struct timer_copy_state {
  bool reject_copies = false;
  bool called = false;
};

struct copy_sensitive_timer {
  timer_copy_state* state;

  explicit copy_sensitive_timer(timer_copy_state& value) : state(&value) {}
  copy_sensitive_timer(copy_sensitive_timer&&) noexcept = default;
  copy_sensitive_timer(const copy_sensitive_timer& other) : state(other.state) {
#ifndef CORO_DISABLE_EXCEPTION
    if (state->reject_copies) {
      throw std::runtime_error("queued timer callback copied");
    }
#else
    ASSERT(!state->reject_copies);
#endif
  }

  void operator()() {
    state->called = true;
  }
};

void test_poll_moves_delayed_callback() {
  executor_poll executor;
  timer_copy_state state;
  executor.post_delayed_ns(copy_sensitive_timer{state}, 0);
  state.reject_copies = true;
  bool copy_failed = false;
#ifndef CORO_DISABLE_EXCEPTION
  try {
#endif
    executor.poll();
#ifndef CORO_DISABLE_EXCEPTION
  } catch (const std::runtime_error&) {
    copy_failed = true;
  }
#endif
  ASSERT(!copy_failed);
  ASSERT(state.called);
}

template <typename BASE>
struct observed_shutdown_executor : BASE {
  std::atomic<bool> dispatch_started{false};
  std::atomic<bool> dispatch_finished{false};
  std::atomic<bool> release_dispatch{false};

  void dispatch(std::function<void()> fn) override {
    dispatch_started.store(true, std::memory_order_release);
    BASE::dispatch(std::move(fn));
    // Hold a producer inside dispatch after the stop callback can run.
    while (!release_dispatch.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    dispatch_finished.store(true, std::memory_order_release);
  }
};

template <typename BASE>
void test_delay_stop_finishes_with_executor(int wait_ms) {
  observed_shutdown_executor<BASE> executor;
  auto started = std::chrono::steady_clock::now();
  delay_stop(executor, wait_ms);
  if constexpr (std::is_same_v<BASE, executor_loop>) {
    executor.run_loop();
  } else {
    while (!executor.stopped()) {
      executor.poll();
      std::this_thread::yield();
    }
  }
  bool producer_active = executor.dispatch_started.load(std::memory_order_acquire) && !executor.dispatch_finished.load(std::memory_order_acquire);
  executor.release_dispatch.store(true, std::memory_order_release);
  if (executor.dispatch_started.load(std::memory_order_acquire)) {
    while (!executor.dispatch_finished.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
  ASSERT(!producer_active);
  if (wait_ms > 0) {
    ASSERT(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(wait_ms));
  }
}

void test_stop_wakes_idle_loop() {
  for (int iteration = 0; iteration < 256; ++iteration) {
    executor_loop executor;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    executor.post([&started] {
      started.store(true, std::memory_order_release);
    });
    std::thread worker([&] {
      executor.run_loop();
      finished.store(true, std::memory_order_release);
    });
    while (!started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    executor.stop();
    auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    bool stopped_on_time = finished.load(std::memory_order_acquire);
    if (!stopped_on_time) {
      // A second notification lets a lost-wakeup failure join cleanly.
      executor.stop();
    }
    worker.join();
    ASSERT(stopped_on_time);
  }
}

void test_stop_wakes_all_idle_loop_threads(bool stop_from_loop) {
  executor_loop executor;
  constexpr int worker_count = 4;
  std::atomic<int> entered{0};
  std::atomic<int> callbacks_finished{0};
  std::atomic<int> loops_finished{0};
  auto deadline = std::chrono::steady_clock::now() + 5s;
  for (int i = 0; i < worker_count; ++i) {
    executor.post([&] {
      entered.fetch_add(1, std::memory_order_release);
      while (entered.load(std::memory_order_acquire) != worker_count && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
      }
      ASSERT(entered.load(std::memory_order_acquire) == worker_count);
      callbacks_finished.fetch_add(1, std::memory_order_release);
    });
  }
  std::vector<std::thread> workers;
  for (int i = 0; i < worker_count; ++i) {
    workers.emplace_back([&] {
      executor.run_loop();
      loops_finished.fetch_add(1, std::memory_order_release);
    });
  }
  while (callbacks_finished.load(std::memory_order_acquire) != worker_count && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT(callbacks_finished.load(std::memory_order_acquire) == worker_count);
  // Let all workers enter the empty-queue wait before issuing one stop request.
  std::this_thread::sleep_for(20ms);
  if (stop_from_loop) {
    executor.post([&] {
      executor.stop();
    });
  } else {
    executor.stop();
  }
  deadline = std::chrono::steady_clock::now() + 500ms;
  while (loops_finished.load(std::memory_order_acquire) != worker_count && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  bool stopped_on_time = loops_finished.load(std::memory_order_acquire) == worker_count;
  if (!stopped_on_time) {
    // Rescue workers left asleep by a single-notification implementation before asserting.
    for (int i = 0; i < worker_count; ++i) {
      executor.stop();
    }
  }
  for (auto& worker : workers) {
    worker.join();
  }
  ASSERT(stopped_on_time);
}

void test_loop_releases_thread_context() {
  executor_loop executor;
  executor.post([&] {
    executor.stop();
  });
  executor.run_loop();
  auto token = std::make_shared<int>(42);
  std::weak_ptr<int> retained = token;
  executor.post_continuation([token] {
    ASSERT(false);
  });
  token.reset();
  // A loop that has finished draining must reject and release new continuations.
  ASSERT(retained.expired());
}

void test_nested_loop_preserves_thread_context() {
  executor_loop executor;
  int completed = 0;
  executor.post([&] {
    executor.post([&] {
      executor.stop();
    });
    executor.run_loop();
    // The enclosing run_loop is still executing this accepted callback.
    executor.post_continuation([&] {
      ++completed;
    });
    ++completed;
  });
  executor.run_loop();
  ASSERT(completed == 2);
}

void test_nested_distinct_loop_context() {
  executor_loop outer;
  executor_loop inner;
  int dispatched = 0;
  outer.post([&] {
    inner.post([&] {
      outer.dispatch([&] {
        ++dispatched;
      });
      ASSERT(dispatched == 1);
      inner.stop();
    });
    inner.run_loop();
    outer.dispatch([&] {
      ++dispatched;
    });
    ASSERT(dispatched == 2);
    outer.stop();
  });
  outer.run_loop();
  ASSERT(dispatched == 2);
}

void test_concurrent_loop_thread_context() {
  executor_loop executor;
  std::atomic<int> entered{0};
  std::atomic<int> completed{0};
  auto deadline = std::chrono::steady_clock::now() + 5s;
  auto task = [&] {
    entered.fetch_add(1, std::memory_order_release);
    while (entered.load(std::memory_order_acquire) != 2 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    ASSERT(entered.load(std::memory_order_acquire) == 2);
    executor.stop();
    executor.post_continuation([&] {
      completed.fetch_add(1, std::memory_order_relaxed);
    });
  };
  executor.post(task);
  executor.post(task);
  std::thread first([&] {
    executor.run_loop();
  });
  std::thread second([&] {
    executor.run_loop();
  });
  first.join();
  second.join();
  ASSERT(completed.load(std::memory_order_relaxed) == 2);
}

#ifndef CORO_DISABLE_EXCEPTION
void test_loop_exception_releases_thread_context() {
  executor_loop executor;
  executor.post([] {
    throw std::runtime_error("queued callback failed");
  });
  bool caught = false;
  try {
    executor.run_loop();
  } catch (const std::runtime_error&) {
    caught = true;
  }
  ASSERT(caught);
  bool called = false;
  executor.dispatch([&] {
    called = true;
  });
  ASSERT(!called);
  executor.post([&] {
    executor.stop();
  });
  executor.run_loop();
  ASSERT(called);
}
#endif

void test_stopped_loop_waits_for_timer() {
  executor_loop executor;
  bool called = false;
  auto started = std::chrono::steady_clock::now();
#ifndef _WIN32
  // MSVC's std::clock measures elapsed wall time instead of process CPU time.
  auto cpu_started = std::clock();
#endif
  executor.post_delayed_ns(
      [&called] {
        called = true;
      },
      200000000);
  executor.stop();
  executor.run_loop();
  auto elapsed = std::chrono::steady_clock::now() - started;
  ASSERT(called);
  ASSERT(elapsed >= 100ms);
#ifndef _WIN32
  auto cpu_elapsed = std::chrono::duration<double>(static_cast<double>(std::clock() - cpu_started) / CLOCKS_PER_SEC);
  ASSERT(cpu_elapsed < elapsed / 2);
#endif
}

void test_post_racing_with_stop(bool delayed) {
  for (int iteration = 0; iteration < 64; ++iteration) {
    executor_loop executor;
    auto token = std::make_shared<int>(42);
    std::weak_ptr<int> retained = token;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::thread worker([&executor] {
      executor.run_loop();
    });
    std::vector<std::thread> producers;
    for (int producer = 0; producer < 4; ++producer) {
      producers.emplace_back([&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        for (int task = 0; task < 64; ++task) {
          auto callback = [token] {
            ASSERT(*token == 42);
          };
          if (delayed) {
            executor.post_delayed_ns(std::move(callback), 0);
          } else {
            executor.post(std::move(callback));
          }
        }
      });
    }
    while (ready.load(std::memory_order_acquire) != 4) {
      std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    std::this_thread::yield();
    executor.stop();
    worker.join();
    for (auto& producer : producers) {
      producer.join();
    }
    token.reset();
    ASSERT(retained.expired());
  }
}

template <typename EXECUTOR>
void test_executor_queue_fairness() {
  for (bool delayed_chain : {false, true}) {
    EXECUTOR exec;
    constexpr int limit = 32;
    int repeated = 0;
    int observed = -1;
    auto schedule = [&](std::function<void()> fn, bool delayed) {
      if (delayed) {
        exec.post_delayed_ns(std::move(fn), 0);
      } else {
        exec.post(std::move(fn));
      }
    };
    auto finish = [&] {
      if (repeated == limit && observed >= 0) {
        exec.stop();
      }
    };
    std::function<void()> repeat;
    repeat = [&] {
      ++repeated;
      if (repeated < limit) {
        schedule(repeat, delayed_chain);
      }
      finish();
    };
    schedule(repeat, delayed_chain);
    schedule(
        [&] {
          observed = repeated;
          finish();
        },
        !delayed_chain);
    if constexpr (std::is_same_v<EXECUTOR, executor_poll>) {
      for (int turn = 0; turn < limit * 2 && !exec.stopped(); ++turn) {
        exec.poll();
      }
    } else {
      exec.run_loop();
    }
    ASSERT(repeated == limit);
    ASSERT(observed >= 0);
    // An endlessly replenished queue must not prevent the other queue from running.
    ASSERT(observed < limit);
  }
}

struct forwarding_executor : executor {
  executor& target_;
  int dispatch_depth_ = 0;

  explicit forwarding_executor(executor& target) : target_(target) {}

  void dispatch(std::function<void()> fn) override {
    ++dispatch_depth_;
    // Distinct executor objects may dispatch inline on the same event loop.
    ASSERT(dispatch_depth_ <= 8);
    target_.dispatch(std::move(fn));
    --dispatch_depth_;
  }

  void post(std::function<void()> fn) override {
    target_.post(std::move(fn));
  }

  void post_delayed_ns(std::function<void()> fn, uint64_t delay_ns) override {
    target_.post_delayed_ns(std::move(fn), delay_ns);
  }
};

template <typename T>
async<T> shared_loop_child(executor& expected_executor, [[maybe_unused]] std::shared_ptr<int> frame_token, bool delayed, [[maybe_unused]] bool fail) {
  ASSERT(co_await current_executor() == &expected_executor);
  if (delayed) {
    co_await sleep(0ns);
  }
#ifndef CORO_DISABLE_EXCEPTION
  if (fail) {
    throw std::runtime_error("shared loop child failed");
  }
#endif
  if constexpr (!std::is_void_v<T>) {
    co_return 42;
  }
}

template <typename T>
async<T> nested_shared_loop_child(executor& current, executor& other, std::shared_ptr<int> frame_token, int depth, bool delayed, bool fail) {
  ASSERT(co_await current_executor() == &current);
  if (depth == 0) {
    co_return co_await shared_loop_child<T>(current, frame_token, delayed, fail);
  }
  if constexpr (std::is_void_v<T>) {
    co_await nested_shared_loop_child<T>(other, current, frame_token, depth - 1, delayed, fail).bind_executor(other);
  } else {
    ASSERT((co_await nested_shared_loop_child<T>(other, current, frame_token, depth - 1, delayed, fail).bind_executor(other)) == 42);
  }
  ASSERT(co_await current_executor() == &current);
  if constexpr (!std::is_void_v<T>) {
    co_return 42;
  }
}

template <typename T>
async<void> await_shared_loop_children(executor* parent_executor, executor& child_executor, bool delayed, bool fail) {
  auto frame_token = std::make_shared<int>(0);
  for (int iteration = 0; iteration < 32; ++iteration) {
    bool caught = false;
#ifndef CORO_DISABLE_EXCEPTION
    try {
#endif
      if constexpr (std::is_void_v<T>) {
        co_await shared_loop_child<T>(child_executor, frame_token, delayed, fail).bind_executor(child_executor);
      } else {
        ASSERT((co_await shared_loop_child<T>(child_executor, frame_token, delayed, fail).bind_executor(child_executor)) == 42);
      }
#ifndef CORO_DISABLE_EXCEPTION
    } catch (const std::runtime_error&) {
      caught = true;
    }
#endif
    ASSERT(caught == fail);
    ASSERT(frame_token.use_count() == 1);
    ASSERT(co_await current_executor() == parent_executor);
  }
  if (parent_executor) {
    bool caught = false;
#ifndef CORO_DISABLE_EXCEPTION
    try {
#endif
      if constexpr (std::is_void_v<T>) {
        co_await nested_shared_loop_child<T>(child_executor, *parent_executor, frame_token, 64, delayed, fail).bind_executor(child_executor);
      } else {
        ASSERT(
            (co_await nested_shared_loop_child<T>(child_executor, *parent_executor, frame_token, 64, delayed, fail).bind_executor(child_executor)) ==
            42);
      }
#ifndef CORO_DISABLE_EXCEPTION
    } catch (const std::runtime_error&) {
      caught = true;
    }
#endif
    ASSERT(caught == fail);
    ASSERT(frame_token.use_count() == 1);
    ASSERT(co_await current_executor() == parent_executor);
  }
}

template <typename T>
void test_executors_sharing_loop() {
  for (bool reverse : {false, true}) {
    for (bool delayed : {false, true}) {
      for (bool fail : {false, true}) {
#ifdef CORO_DISABLE_EXCEPTION
        if (fail) {
          continue;
        }
#endif
        executor_loop loop;
        forwarding_executor alias(loop);
        executor& parent = reverse ? static_cast<executor&>(alias) : loop;
        executor& child = reverse ? static_cast<executor&>(loop) : alias;
        bool completed = false;
        await_shared_loop_children<T>(&parent, child, delayed, fail).detach_with_callback(parent, [&] {
          completed = true;
          loop.stop();
        });
        loop.run_loop();
        ASSERT(completed);
        ASSERT(alias.dispatch_depth_ == 0);
        check_coro_leak();
      }
    }
  }
}

template <typename T>
async<void> await_children_without_parent_executor(executor_loop& loop, executor& child_executor, bool delayed, bool fail) {
  co_await await_shared_loop_children<T>(nullptr, child_executor, delayed, fail);
  loop.stop();
}

template <typename T>
void test_bound_children_with_unbound_parent() {
  for (bool delayed : {false, true}) {
    for (bool fail : {false, true}) {
#ifdef CORO_DISABLE_EXCEPTION
      if (fail) {
        continue;
      }
#endif
      executor_loop loop;
      forwarding_executor child(loop);
      {
        auto task = await_children_without_parent_executor<T>(loop, child, delayed, fail);
        loop.post([&task] {
          task.current_coro_handle_.resume();
        });
        loop.run_loop();
        ASSERT(task.current_coro_handle_.done());
        task.await_resume();
        ASSERT(child.dispatch_depth_ == 0);
      }
      check_coro_leak();
    }
  }
}

void test_time_count_duration() {
  TimeCount<std::chrono::nanoseconds> timer;
  std::this_thread::sleep_for(2ms);
  auto before = timer.elapsed<std::chrono::nanoseconds>();
  auto default_elapsed = timer.elapsed();
  auto after = timer.elapsed<std::chrono::nanoseconds>();
  ASSERT(before <= default_elapsed && default_elapsed <= after);

  before = timer.elapsed();
  auto microseconds = timer.elapsed<std::chrono::microseconds>();
  after = timer.elapsed();
  ASSERT(before / 1000 <= microseconds && microseconds <= after / 1000);
}

int main() {
  for (auto kind : {queued_task_kind::posted, queued_task_kind::timed, queued_task_kind::continuation}) {
    test_loop_destruction_after_published_task(kind);
    for (bool reenter_on_copy : {false, true}) {
      test_queued_callback_reenters_executor<executor_poll>(kind, reenter_on_copy);
      test_queued_callback_reenters_executor<executor_loop>(kind, reenter_on_copy);
    }
  }
#ifndef CORO_DISABLE_EXCEPTION
  test_failed_timer_cleanup_reenters_executor<executor_poll>();
  test_failed_timer_cleanup_reenters_executor<executor_poll>(1);
  test_failed_timer_cleanup_reenters_executor<executor_loop>();
  test_failed_timer_cleanup_reenters_executor<executor_loop>(1);
#endif
  test_time_count_duration();
  test_bound_children_with_unbound_parent<int>();
  test_bound_children_with_unbound_parent<void>();
  test_executors_sharing_loop<int>();
  test_executors_sharing_loop<void>();
  for (int wait_ms : {-1, 0, 5}) {
    test_delay_stop_finishes_with_executor<executor_loop>(wait_ms);
    test_delay_stop_finishes_with_executor<executor_poll>(wait_ms);
  }
  LOG("init");
  test_executor_queue_fairness<executor_poll>();
  test_executor_queue_fairness<executor_loop>();
  test_poll_moves_delayed_callback();
  test_post_racing_with_stop(false);
  test_post_racing_with_stop(true);
  test_stop_wakes_all_idle_loop_threads(false);
  test_stop_wakes_all_idle_loop_threads(true);
  test_loop_releases_thread_context();
  test_nested_loop_preserves_thread_context();
  test_nested_distinct_loop_context();
  test_concurrent_loop_thread_context();
#ifndef CORO_DISABLE_EXCEPTION
  test_loop_exception_releases_thread_context();
#endif
  test_stopped_loop_waits_for_timer();
  test_stop_wakes_idle_loop();
  test_large_delayed_post();
  test_delayed_post_wakes_loop(false);
  test_delayed_post_wakes_loop(true);
#ifdef CORO_EXECUTOR_LOOP
  executor_loop executor;
#elif defined(CORO_EXECUTOR_POLL)
  executor_poll executor;
#endif

  test_coro(executor);
  test_simple(executor);
  delay_stop(executor, 1500);
  LOG("loop...");
#ifdef CORO_EXECUTOR_LOOP
  executor.run_loop();
#elif defined(CORO_EXECUTOR_POLL)
  while (!executor.stopped()) {
    executor.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
#endif
  check_coro_leak();
  return 0;
}
