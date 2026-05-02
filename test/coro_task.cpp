/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include "log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include <atomic>
#include <ctime>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "TimeCount.hpp"
#include "assert_def.h"
#include "coro/coro.hpp"
#include "coro/time.hpp"
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

  void stop() override {
    target_.stop();
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
