/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>

#include "TimeCount.hpp"
#include "assert_def.h"
#include "coro.hpp"
#include "log.h"
#include "utils.hpp"

using namespace coro;

#ifndef CORO_SINGLE_THREAD
void test_when_all_result_publication([[maybe_unused]] bool fail) {
  for (int iteration = 0; iteration < 100; ++iteration) {
    using state_type = detail::when_all_state<int, int>;
    auto state = std::make_shared<state_type>();
    detail::when_all_awaiter<state_type, std::tuple<int, int>> waiter{state};
    std::thread first([state] {
      std::optional<int> result{42};
      state->template set_result<0>(result);
    });
    std::thread second([state, fail] {
#ifndef CORO_DISABLE_EXCEPTION
      if (fail) {
        state->set_exception(std::make_exception_ptr(std::runtime_error("published error")));
        return;
      }
#else
      (void)fail;
#endif
      std::optional<int> result{17};
      state->template set_result<1>(result);
    });
    while (!waiter.await_ready()) {
      std::this_thread::yield();
    }
    // Consume before joining: readiness must publish both writers' non-atomic results.
#ifndef CORO_DISABLE_EXCEPTION
    bool caught = false;
    try {
#endif
      auto values = waiter.await_resume();
      ASSERT(std::get<0>(values) == 42 && std::get<1>(values) == 17);
#ifndef CORO_DISABLE_EXCEPTION
    } catch (const std::runtime_error& error) {
      ASSERT(std::string_view(error.what()) == "published error");
      caught = true;
    }
    ASSERT(caught == fail);
#endif
    first.join();
    second.join();
  }
}
#endif

using all_lock_state = detail::when_all_state<int>;
using any_lock_state = detail::when_any_state<int>;
using any_void_lock_state = detail::when_any_state<void>;
static_assert(!noexcept(std::declval<detail::when_all_awaiter<all_lock_state, std::tuple<>>&>().await_suspend(std::noop_coroutine())));
static_assert(!noexcept(std::declval<detail::when_all_awaiter<all_lock_state, std::tuple<int, int>>&>().await_suspend(std::noop_coroutine())));
static_assert(!noexcept(std::declval<detail::when_all_single_awaiter<all_lock_state, int>&>().await_suspend(std::noop_coroutine())));
static_assert(!noexcept(std::declval<detail::when_all_void_awaiter<all_lock_state>&>().await_suspend(std::noop_coroutine())));
static_assert(
    !noexcept(std::declval<detail::when_any_awaiter<any_lock_state, detail::when_any_result<int>, int>&>().await_suspend(std::noop_coroutine())));
static_assert(!noexcept(
    std::declval<detail::when_any_awaiter<any_void_lock_state, detail::when_any_result<void>, void>&>().await_suspend(std::noop_coroutine())));

struct nested_when_executor : executor_poll {
  int dispatch_depth = 0;
  int max_dispatch_depth = 0;

  void dispatch(std::function<void()> task) override {
    ++dispatch_depth;
    if (dispatch_depth > max_dispatch_depth) {
      max_dispatch_depth = dispatch_depth;
    }
    executor_poll::dispatch(std::move(task));
    --dispatch_depth;
  }
};

template <bool ANY, typename T>
async<T> nested_when_task(int depth, bool delayed, bool fail, std::shared_ptr<int> frame_token) {
  if (depth == 0) {
    if (delayed) {
      co_await sleep(0ms);
    }
#ifndef CORO_DISABLE_EXCEPTION
    if (fail) {
      throw std::runtime_error("nested child failed");
    }
#endif
    if constexpr (std::is_void_v<T>) {
      co_return;
    } else {
      co_return 42;
    }
  }
  if constexpr (ANY) {
    auto result = co_await when_any(nested_when_task<ANY, T>(depth - 1, delayed, fail, frame_token));
    ASSERT(result.index == 0);
    if constexpr (!std::is_void_v<T>) {
      co_return result.template get<0>();
    }
  } else {
    if constexpr (std::is_void_v<T>) {
      co_await when_all(nested_when_task<ANY, T>(depth - 1, delayed, fail, frame_token));
    } else {
      co_return co_await when_all(nested_when_task<ANY, T>(depth - 1, delayed, fail, frame_token));
    }
  }
}

template <bool ANY, typename T>
async<void> await_nested_when(bool delayed, bool fail, std::shared_ptr<int> frame_token) {
#ifndef CORO_DISABLE_EXCEPTION
  bool caught = false;
  try {
#endif
    if constexpr (std::is_void_v<T>) {
      co_await nested_when_task<ANY, T>(128, delayed, fail, frame_token);
    } else {
      ASSERT((co_await nested_when_task<ANY, T>(128, delayed, fail, frame_token)) == 42);
    }
#ifndef CORO_DISABLE_EXCEPTION
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "nested child failed");
    caught = true;
  }
  ASSERT(caught == fail);
#endif
}

template <bool ANY, typename T>
void test_nested_when() {
  for (bool delayed : {false, true}) {
    for (bool fail : {false, true}) {
#ifdef CORO_DISABLE_EXCEPTION
      if (fail) {
        continue;
      }
#endif
      nested_when_executor executor;
      auto frame_token = std::make_shared<int>(0);
      bool completed = false;
      await_nested_when<ANY, T>(delayed, fail, frame_token).detach_with_callback(executor, [&] {
        completed = true;
      });
      for (int attempt = 0; !completed && attempt < 10000; ++attempt) {
        executor.poll();
      }
      ASSERT(completed);
      ASSERT(executor.max_dispatch_depth <= 8);
      ASSERT(frame_token.use_count() == 1);
      check_coro_leak();
    }
  }
}

#ifndef CORO_DISABLE_EXCEPTION
struct rejected_when_executor : executor_poll {
  bool reject_next_post = false;

  void post(std::function<void()> task) override {
    if (std::exchange(reject_next_post, false)) {
      throw std::bad_alloc();
    }
    executor_poll::post(std::move(task));
  }
};

async<int> when_launch_probe(bool& started, std::shared_ptr<int> frame_token) {
  started = true;
  co_return *frame_token;
}

template <bool ANY>
async<void> await_rejected_when_launch(rejected_when_executor& executor, bool& completed) {
  auto frame_token = std::make_shared<int>(42);
  bool started = false;
  bool caught = false;
  executor.reject_next_post = true;
  try {
    if constexpr (ANY) {
      co_await when_any(when_launch_probe(started, frame_token));
    } else {
      co_await when_all(when_launch_probe(started, frame_token));
    }
  } catch (const std::bad_alloc&) {
    caught = true;
  }
  ASSERT(caught);
  ASSERT(!started);
  ASSERT(frame_token.use_count() == 1);
  // An empty combiner has no children to schedule.
  executor.reject_next_post = true;
  co_await when_all();
  ASSERT(executor.reject_next_post);
  executor.reject_next_post = false;
  completed = true;
}

template <bool ANY>
void test_rejected_when_launch() {
  rejected_when_executor executor;
  bool completed = false;
  spawn(executor, await_rejected_when_launch<ANY>(executor, completed));
  for (int attempt = 0; !completed && attempt < 100; ++attempt) {
    executor.poll();
  }
  ASSERT(completed);
  check_coro_leak();
}
#endif

struct reentrant_result_state {
  channel<int> signal;
  bool close_on_move = false;
  bool throw_on_move = false;
  int live = 0;
};

struct reentrant_result {
  reentrant_result_state* state;
  int generation = 0;

  explicit reentrant_result(reentrant_result_state& result_state) : state(&result_state) {
    ++state->live;
  }
  reentrant_result(const reentrant_result&) = delete;
  reentrant_result(reentrant_result&& other) : state(other.state), generation(other.generation + 1) {
    // The fourth move stores the wrapper's result in the combiner's shared state.
    if (state->close_on_move && generation == 4) {
      state->signal.close();
    }
#ifndef CORO_DISABLE_EXCEPTION
    if (state->throw_on_move && generation == 4) {
      throw std::runtime_error("reentrant result storage failed");
    }
#endif
    ++state->live;
  }
  ~reentrant_result() {
    --state->live;
    // The wrapper releases its moved-from result before publishing completion.
    if (!state->close_on_move && generation == 3) {
      state->signal.close();
    }
  }
};

async<void> wait_for_reentrant_signal(channel<int>& signal) {
  ASSERT(!(co_await signal.recv()));
}

async<reentrant_result> make_reentrant_result(reentrant_result_state& state, bool delayed) {
  if (delayed) {
    co_await sleep(0ms);
  }
  co_return reentrant_result{state};
}

async<void> await_reentrant_result(reentrant_result_state& state, bool delayed) {
  auto result = co_await when_all(wait_for_reentrant_signal(state.signal), make_reentrant_result(state, delayed));
  ASSERT(result.state == &state);
  ASSERT(state.signal.is_closed());
}

async<void> await_reentrant_any_result(reentrant_result_state& state, bool delayed) {
#ifndef CORO_DISABLE_EXCEPTION
  bool caught = false;
  try {
#endif
    auto result = co_await when_any(wait_for_reentrant_signal(state.signal), make_reentrant_result(state, delayed));
    ASSERT(result.index == 1);
    ASSERT(result.get<1>().state == &state);
#ifndef CORO_DISABLE_EXCEPTION
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "reentrant result storage failed");
    caught = true;
  }
  ASSERT(caught == state.throw_on_move);
#endif
  ASSERT(state.signal.is_closed());
}

void run_reentrant_test(async<void> task) {
  executor_loop executor;
  std::atomic<bool> completed{false};
  task.detach_with_callback(executor, [&] {
    completed.store(true, std::memory_order_release);
    executor.stop();
  });
  std::thread worker([&executor] {
    executor.run_loop();
  });
  auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!completed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  ASSERT(completed.load(std::memory_order_acquire));
  worker.join();
  check_coro_leak();
}

#ifndef CORO_DISABLE_EXCEPTION
struct reentrant_error {
  channel<int>* signal;
  ~reentrant_error() {
    signal->close();
  }
};

async<void> first_reentrant_error() {
  throw std::runtime_error("first completion error");
  co_return;
}

async<void> losing_reentrant_error(channel<int>& signal, bool delayed) {
  if (delayed) {
    co_await sleep(0ms);
  }
  throw reentrant_error{&signal};
}

async<void> await_reentrant_error(channel<int>& signal, bool delayed) {
  bool caught = false;
  try {
    co_await when_all(first_reentrant_error(), wait_for_reentrant_signal(signal), losing_reentrant_error(signal, delayed));
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "first completion error");
    caught = true;
  }
  ASSERT(caught);
  ASSERT(signal.is_closed());
}
#endif

void test_when_all_reentrant_completion() {
  for (bool delayed : {false, true}) {
    for (bool close_on_move : {false, true}) {
      LOG("when_all reentrant result: move=%d delayed=%d", close_on_move, delayed);
      reentrant_result_state state;
      state.close_on_move = close_on_move;
      run_reentrant_test(await_reentrant_result(state, delayed));
      ASSERT(state.live == 0);
    }
#ifndef CORO_DISABLE_EXCEPTION
    LOG("when_all reentrant error: delayed=%d", delayed);
    channel<int> signal;
    run_reentrant_test(await_reentrant_error(signal, delayed));
#endif
  }
}

void test_when_any_reentrant_completion() {
  for (bool delayed : {false, true}) {
    for (bool close_on_move : {false, true}) {
      LOG("when_any reentrant result: move=%d delayed=%d", close_on_move, delayed);
      reentrant_result_state state;
      state.close_on_move = close_on_move;
      run_reentrant_test(await_reentrant_any_result(state, delayed));
      ASSERT(state.live == 0);
#ifndef CORO_DISABLE_EXCEPTION
      reentrant_result_state throwing_state;
      throwing_state.close_on_move = close_on_move;
      throwing_state.throw_on_move = true;
      run_reentrant_test(await_reentrant_any_result(throwing_state, delayed));
      ASSERT(throwing_state.live == 0);
#endif
    }
  }
}

// Hold the winning result's move constructor open while the parent checks readiness.
#ifndef CORO_SINGLE_THREAD
struct publication_value {
  std::atomic<bool>* entered;
  std::atomic<bool>* release;
  int value;

  publication_value(std::atomic<bool>& entered_flag, std::atomic<bool>& release_flag, int v)
      : entered(&entered_flag), release(&release_flag), value(v) {}

  publication_value(publication_value&& other) noexcept : entered(other.entered), release(other.release), value(other.value) {
    entered->store(true, std::memory_order_release);
    while (!release->load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  publication_value(const publication_value&) = delete;
};

void test_when_any_result_publication() {
  using state_type = detail::when_any_state<int, publication_value>;
  using result_type = detail::when_any_result<int, publication_value>;
  auto state = std::make_shared<state_type>();
  detail::when_any_awaiter<state_type, result_type, int, publication_value> waiter{state};
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::optional<publication_value> value{std::in_place, entered, release, 42};

  std::thread writer([&] {
    state->template set_result<1>(value);
  });
  while (!entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  bool ready_before_result = waiter.await_ready();
  // Other completions must neither block on the user's move nor replace the claimed result.
  std::optional<int> losing_value{17};
  state->template set_result<0>(losing_value);
#ifndef CORO_DISABLE_EXCEPTION
  state->set_exception(std::make_exception_ptr(std::runtime_error("losing completion")));
#endif
  ASSERT(!waiter.await_ready());
  release.store(true, std::memory_order_release);
  writer.join();

  ASSERT(!ready_before_result);
  ASSERT(waiter.await_ready());
  auto result = waiter.await_resume();
  ASSERT(result.index == 1);
  ASSERT(result.template get<1>().value == 42);
  LOG("when_any result publication test: PASSED");
}
#endif

// Optional counters track completion on the single test executor. Shared ownership keeps them
// alive for when_any branches that outlive their caller.
// Test task that returns a value after a delay
async<int> delayed_value_task(int value, int delay_ms, std::shared_ptr<int> completed = {}) {
  LOG("Task %d: starting, will complete after %d ms", value, delay_ms);
  co_await sleep(std::chrono::milliseconds(delay_ms));
  if (completed) {
    ++*completed;
  }
  LOG("Task %d: completed", value);
  co_return value;
}

// Test task that returns void
async<void> delayed_void_task(const char* name, int delay_ms, std::shared_ptr<int> completed = {}) {
  LOG("Task %s: starting, will complete after %d ms", name, delay_ms);
  co_await sleep(std::chrono::milliseconds(delay_ms));
  if (completed) {
    ++*completed;
  }
  LOG("Task %s: completed", name);
}

async<int> immediate_value_task(int value) {
  LOG("Immediate task %d: completed", value);
  co_return value;
}

async<void> immediate_void_task(int& marker, int bit) {
  marker |= bit;
  LOG("Immediate void task: marker=%d", marker);
  co_return;
}

async<int> delayed_counter_task(std::atomic<int>& counter, int value, int delay_ms) {
  co_await sleep(std::chrono::milliseconds(delay_ms));
  counter.fetch_add(value, std::memory_order_relaxed);
  LOG("Counter task %d: completed", value);
  co_return value;
}

// Test when_all with multiple tasks returning values
async<void> test_when_all_values() {
  LOG("=== Testing when_all with value-returning tasks ===");
  TimeCount t;

  // Launch 3 tasks with different delays
  auto results = co_await when_all(delayed_value_task(1, 50), delayed_value_task(2, 30), delayed_value_task(3, 40));

  auto elapsed = t.elapsed();
  LOG("All tasks completed after %d ms", (int)elapsed);

  // Extract results from tuple
  auto [r1, r2, r3] = results;
  LOG("Results: %d, %d, %d", r1, r2, r3);

  ASSERT(r1 == 1);
  ASSERT(r2 == 2);
  ASSERT(r3 == 3);
  ASSERT(elapsed >= 50);  // Should wait for the longest task (50ms)

  LOG("when_all values test: PASSED");
}

// Test when_all with void tasks
async<void> test_when_all_void() {
  LOG("=== Testing when_all with void tasks ===");
  TimeCount t;

  // Launch 3 void tasks with different delays
  co_await when_all(delayed_void_task("A", 30), delayed_void_task("B", 20), delayed_void_task("C", 40));

  auto elapsed = t.elapsed();
  LOG("All void tasks completed after %d ms", (int)elapsed);
  ASSERT(elapsed >= 40);  // Should wait for the longest task (40ms)

  LOG("when_all void test: PASSED");
}

// Test when_any with multiple tasks
async<void> test_when_any_values() {
  LOG("=== Testing when_any with value-returning tasks ===");
  TimeCount t;
  auto completed = std::make_shared<int>(0);
  // Queue the stall from a child so the combiner launches all timers before the loop blocks.
  auto busy_task = [](std::shared_ptr<int> completed) -> async<int> {
    (co_await current_executor())->post([] {
      std::this_thread::sleep_for(100ms);
    });
    co_return co_await delayed_value_task(1, 100, completed);
  };

  // Launch 3 tasks with different delays
  auto result = co_await when_any(busy_task(completed), delayed_value_task(2, 30, completed), delayed_value_task(3, 80, completed));

  auto elapsed = t.elapsed();
  LOG("First task completed after %d ms", (int)elapsed);
  LOG("Completed task index: %zu", result.index);

  // Task 2 should complete first (30ms delay)
  ASSERT(result.index == 1);  // Index 1 corresponds to the second task
  ASSERT(elapsed >= 30);
  ASSERT(*completed == 1);  // Returning must not wait for the slower branches.
  // Get the value from the result
  auto value = result.template get<1>();
  LOG("Completed task value: %d", value);
  ASSERT(value == 2);

  LOG("when_any values test: PASSED");
}

// Test when_any with void tasks
async<void> test_when_any_void() {
  LOG("=== Testing when_any with void tasks ===");
  TimeCount t;
  auto completed = std::make_shared<int>(0);

  // Launch 3 void tasks with different delays
  auto result =
      co_await when_any(delayed_void_task("A", 60, completed), delayed_void_task("B", 20, completed), delayed_void_task("C", 50, completed));

  auto elapsed = t.elapsed();
  LOG("First void task completed after %d ms", (int)elapsed);
  LOG("Completed task index: %zu", result.index);

  // Task B should complete first (20ms delay)
  ASSERT(result.index == 1);  // Index 1 corresponds to task B
  ASSERT(elapsed >= 20);
  ASSERT(*completed == 1);  // Returning must not wait for the slower branches.

  LOG("when_any void test: PASSED");
}

// Test when_all with mixed types (int, void, int)
async<void> test_when_all_mixed_types() {
  LOG("=== Testing when_all with mixed types (int, void, int) ===");
  TimeCount t;

  // Launch tasks with different return types: int, void, int
  auto results = co_await when_all(delayed_value_task(42, 30), delayed_void_task("MixedVoid", 20), delayed_value_task(99, 25));

  auto elapsed = t.elapsed();
  LOG("All mixed tasks completed after %d ms", (int)elapsed);

  // Results should only contain non-void values: (42, 99)
  auto [r1, r2] = results;
  LOG("Non-void results: %d, %d", r1, r2);

  ASSERT(r1 == 42);
  ASSERT(r2 == 99);
  ASSERT(elapsed >= 30);  // Should wait for the longest task

  LOG("when_all mixed types test: PASSED");
}

// Test when_all with mixed types (void, int, void)
async<void> test_when_all_mixed_types2() {
  LOG("=== Testing when_all with mixed types (void, int, void) ===");
  TimeCount t;

  // Launch tasks: void, int, void
  auto result = co_await when_all(delayed_void_task("VoidA", 15), delayed_value_task(123, 20), delayed_void_task("VoidB", 10));

  auto elapsed = t.elapsed();
  LOG("All mixed tasks completed after %d ms", (int)elapsed);

  // Result should be a single int (not wrapped in tuple for single non-void)
  LOG("Non-void result: %d", result);

  ASSERT(result == 123);
  ASSERT(elapsed >= 20);

  LOG("when_all mixed types test 2: PASSED");
}

// Test when_any with mixed types
async<void> test_when_any_mixed_types() {
  LOG("=== Testing when_any with mixed types (int, void, int) ===");
  TimeCount t;
  auto completed = std::make_shared<int>(0);

  // Launch tasks with different return types
  auto result =
      co_await when_any(delayed_value_task(100, 50, completed), delayed_void_task("FastVoid", 15, completed), delayed_value_task(200, 60, completed));

  auto elapsed = t.elapsed();
  LOG("First task completed after %d ms", (int)elapsed);
  LOG("Completed task index: %zu", result.index);

  // The void task (index 1) should complete first
  ASSERT(result.index == 1);
  ASSERT(elapsed >= 15);
  ASSERT(*completed == 1);  // Returning must not wait for the slower branches.

  LOG("when_any mixed types test: PASSED");
}

async<void> test_when_all_immediate_completion() {
  LOG("=== Testing when_all with immediately-completed tasks ===");

  int marker = 0;
  auto results = co_await when_all(immediate_value_task(10), immediate_void_task(marker, 1), immediate_value_task(20));
  auto [r1, r2] = results;

  ASSERT(r1 == 10);
  ASSERT(r2 == 20);
  ASSERT(marker == 1);

  LOG("when_all immediate completion test: PASSED");
}

async<void> test_when_all_empty_completion() {
  LOG("=== Testing when_all with no tasks ===");

  auto results = co_await when_all();
  static_assert(std::is_same_v<decltype(results), std::tuple<>>);
  ASSERT(std::tuple_size_v<decltype(results)> == 0);

  LOG("when_all empty completion test: PASSED");
}

async<void> test_when_any_immediate_completion() {
  LOG("=== Testing when_any with immediately-completed first task ===");

  auto result = co_await when_any(immediate_value_task(7), delayed_value_task(8, 30));

  ASSERT(result.index == 0);
  ASSERT(result.template get<0>() == 7);

  // when_any does not cancel slower tasks; let the delayed task finish before leak checks.
  co_await sleep(40ms);

  LOG("when_any immediate completion test: PASSED");
}

async<void> test_when_any_keeps_losers_running() {
  LOG("=== Testing when_any leaves slower tasks running ===");

  std::atomic<int> completed_sum{0};
  auto result = co_await when_any(delayed_counter_task(completed_sum, 1, 10), delayed_counter_task(completed_sum, 2, 35),
                                  delayed_counter_task(completed_sum, 4, 55));

  ASSERT(result.index == 0);
  ASSERT(result.template get<0>() == 1);
  ASSERT(completed_sum.load(std::memory_order_relaxed) == 1);

  co_await sleep(70ms);
  ASSERT(completed_sum.load(std::memory_order_relaxed) == 7);

  LOG("when_any slower tasks running test: PASSED");
}

#ifndef CORO_DISABLE_EXCEPTION

// Exception test tasks
async<int> delayed_value_task_with_exception(int value, int delay_ms, bool should_throw, std::shared_ptr<int> completed = {}) {
  LOG("Task %d: starting, will complete after %d ms, throw=%d", value, delay_ms, should_throw);
  co_await sleep(std::chrono::milliseconds(delay_ms));
  if (completed) {
    ++*completed;
  }
  if (should_throw) {
    LOG("Task %d: throwing exception", value);
    throw std::runtime_error("Task " + std::to_string(value) + " failed");
  }
  LOG("Task %d: completed", value);
  co_return value;
}

async<void> delayed_void_task_with_exception(const char* name, int delay_ms, bool should_throw, std::shared_ptr<int> completed = {}) {
  LOG("Task %s: starting, will complete after %d ms, throw=%d", name, delay_ms, should_throw);
  co_await sleep(std::chrono::milliseconds(delay_ms));
  if (completed) {
    ++*completed;
  }
  if (should_throw) {
    LOG("Task %s: throwing exception", name);
    throw std::runtime_error(std::string("Task ") + name + " failed");
  }
  LOG("Task %s: completed", name);
}

// Test when_all with exception (one task throws)
async<void> test_when_all_exception() {
  LOG("=== Testing when_all with exception ===");
  TimeCount t;

  bool exception_caught = false;
  std::string exception_msg;
  try {
    // Task 2 will throw after 30ms, but when_all waits for all tasks (50ms total)
    co_await when_all(delayed_value_task_with_exception(1, 50, false), delayed_value_task_with_exception(2, 30, true),
                      delayed_value_task_with_exception(3, 40, false));
    // Should never reach here
    LOG("ERROR: Should have thrown exception");
    ASSERT(false && "when_all should have thrown exception");
  } catch (const std::runtime_error& e) {
    exception_msg = e.what();
    LOG("Caught expected exception: %s", exception_msg.c_str());
    exception_caught = true;
  }

  auto elapsed = t.elapsed();
  LOG("Exception thrown after %d ms", (int)elapsed);

  ASSERT(exception_caught);
  ASSERT(exception_msg.find("Task 2") != std::string::npos);  // Verify it's from task 2
  ASSERT(elapsed >= 50);                                      // when_all waits for all tasks to complete
  LOG("when_all exception test: PASSED");
}

// Test when_all with void tasks and exception
async<void> test_when_all_void_exception() {
  LOG("=== Testing when_all with void tasks and exception ===");
  TimeCount t;

  bool exception_caught = false;
  std::string exception_msg;
  try {
    // Task B will throw after 20ms, but when_all waits for all tasks (40ms total)
    co_await when_all(delayed_void_task_with_exception("A", 30, false), delayed_void_task_with_exception("B", 20, true),
                      delayed_void_task_with_exception("C", 40, false));
    // Should never reach here
    LOG("ERROR: Should have thrown exception");
    ASSERT(false && "when_all should have thrown exception");
  } catch (const std::runtime_error& e) {
    exception_msg = e.what();
    LOG("Caught expected exception: %s", exception_msg.c_str());
    exception_caught = true;
  }

  auto elapsed = t.elapsed();
  LOG("Exception thrown after %d ms", (int)elapsed);

  ASSERT(exception_caught);
  ASSERT(exception_msg.find("Task B") != std::string::npos);  // Verify it's from task B
  ASSERT(elapsed >= 40);                                      // when_all waits for all tasks to complete
  LOG("when_all void exception test: PASSED");
}

// Test when_all with mixed types and exception
async<void> test_when_all_mixed_exception() {
  LOG("=== Testing when_all with mixed types and exception ===");
  TimeCount t;

  bool exception_caught = false;
  std::string exception_msg;
  try {
    // Value task will throw after 20ms, but when_all waits for all (20ms total)
    [[maybe_unused]] auto result =
        co_await when_all(delayed_void_task_with_exception("VoidA", 15, false), delayed_value_task_with_exception(123, 20, true),
                          delayed_void_task_with_exception("VoidB", 10, false));
    // Should never reach here
    LOG("ERROR: Should have thrown exception");
    ASSERT(false && "when_all should have thrown exception");
  } catch (const std::runtime_error& e) {
    exception_msg = e.what();
    LOG("Caught expected exception: %s", exception_msg.c_str());
    exception_caught = true;
  }

  auto elapsed = t.elapsed();
  LOG("Exception thrown after %d ms", (int)elapsed);

  ASSERT(exception_caught);
  ASSERT(exception_msg.find("Task 123") != std::string::npos);  // Verify it's from task 123
  ASSERT(elapsed >= 20);                                        // when_all waits for all tasks to complete
  LOG("when_all mixed exception test: PASSED");
}

// Test when_any with exception (first completed task throws)
async<void> test_when_any_exception_first() {
  LOG("=== Testing when_any with exception (first task throws) ===");
  TimeCount t;
  auto completed = std::make_shared<int>(0);

  bool exception_caught = false;
  std::string exception_msg;
  try {
    // Task 2 completes first (20ms) but throws
    [[maybe_unused]] auto result =
        co_await when_any(delayed_value_task_with_exception(1, 100, false, completed), delayed_value_task_with_exception(2, 20, true, completed),
                          delayed_value_task_with_exception(3, 80, false, completed));
    // Should never reach here
    LOG("ERROR: Should have thrown exception");
    ASSERT(false && "when_any should have thrown exception");
  } catch (const std::runtime_error& e) {
    exception_msg = e.what();
    LOG("Caught expected exception: %s", exception_msg.c_str());
    exception_caught = true;
  }

  auto elapsed = t.elapsed();
  LOG("Exception thrown after %d ms", (int)elapsed);

  ASSERT(exception_caught);
  ASSERT(exception_msg.find("Task 2") != std::string::npos);  // Verify it's from task 2
  ASSERT(elapsed >= 20);
  ASSERT(*completed == 1);  // Returning must not wait for the slower branches.
  LOG("when_any exception first test: PASSED");
}

// Test when_any with exception (first completed task doesn't throw)
async<void> test_when_any_exception_later() {
  LOG("=== Testing when_any with exception (later task throws) ===");
  TimeCount t;
  auto completed = std::make_shared<int>(0);

  // Task 2 completes first and succeeds, task 3 throws later (but is ignored)
  auto result =
      co_await when_any(delayed_value_task_with_exception(1, 100, true, completed), delayed_value_task_with_exception(2, 20, false, completed),
                        delayed_value_task_with_exception(3, 80, true, completed));

  auto elapsed = t.elapsed();
  LOG("First task completed after %d ms", (int)elapsed);
  LOG("Completed task index: %zu", result.index);

  // Task 2 should complete first (20ms delay) without exception
  ASSERT(result.index == 1);
  ASSERT(elapsed >= 20);
  ASSERT(*completed == 1);  // Returning must not wait for the slower branches.

  auto value = result.template get<1>();
  LOG("Completed task value: %d", value);
  ASSERT(value == 2);

  LOG("when_any exception later test: PASSED");
}

// Test when_any with void tasks and exception
async<void> test_when_any_void_exception() {
  LOG("=== Testing when_any with void tasks and exception ===");
  TimeCount t;
  auto completed = std::make_shared<int>(0);

  bool exception_caught = false;
  std::string exception_msg;
  try {
    // Task B completes first (20ms) and throws
    [[maybe_unused]] auto result =
        co_await when_any(delayed_void_task_with_exception("A", 60, false, completed), delayed_void_task_with_exception("B", 20, true, completed),
                          delayed_void_task_with_exception("C", 50, false, completed));
    // Should never reach here
    LOG("ERROR: Should have thrown exception");
    ASSERT(false && "when_any should have thrown exception");
  } catch (const std::runtime_error& e) {
    exception_msg = e.what();
    LOG("Caught expected exception: %s", exception_msg.c_str());
    exception_caught = true;
  }

  auto elapsed = t.elapsed();
  LOG("Exception thrown after %d ms", (int)elapsed);

  ASSERT(exception_caught);
  ASSERT(exception_msg.find("Task B") != std::string::npos);  // Verify it's from task B
  ASSERT(elapsed >= 20);
  ASSERT(*completed == 1);  // Returning must not wait for the slower branches.
  LOG("when_any void exception test: PASSED");
}

// Test when_all with multiple exceptions (first exception should be thrown)
async<void> test_when_all_multiple_exceptions() {
  LOG("=== Testing when_all with multiple exceptions ===");
  TimeCount t;

  bool exception_caught = false;
  std::string exception_msg;
  try {
    // Multiple tasks throw, first one (by time) should be caught
    [[maybe_unused]] auto results =
        co_await when_all(delayed_value_task_with_exception(1, 50, true), delayed_value_task_with_exception(2, 20, true),  // This one throws first
                          delayed_value_task_with_exception(3, 40, true));
    // Should never reach here
    LOG("ERROR: Should have thrown exception");
    ASSERT(false && "when_all should have thrown exception");
  } catch (const std::runtime_error& e) {
    exception_msg = e.what();
    LOG("Caught expected exception: %s", exception_msg.c_str());
    exception_caught = true;
  }

  auto elapsed = t.elapsed();
  LOG("Exception thrown after %d ms", (int)elapsed);

  ASSERT(exception_caught);
  // Should be from task 2 (completes/throws first at 20ms)
  ASSERT(exception_msg.find("Task 2") != std::string::npos);
  ASSERT(elapsed >= 50);  // when_all waits for all tasks to complete
  LOG("when_all multiple exceptions test: PASSED");
}

#endif

// Exercise cooperative stop through a nested combiner, without timer ordering.
template <typename EVENT, typename GROUP>
async<void> stopped_when_branch(EVENT& stop, EVENT& ready, GROUP& finished, int& started, int& cleaned) {
  struct completion_guard {
    GROUP& finished;
    int& cleaned;
    ~completion_guard() {
      ++cleaned;
      finished.done();
    }
  } guard{finished, cleaned};
  if (++started == 2) {
    ready.set();
  }
  co_await stop.wait();
  ASSERT(stop.is_set());
}

template <typename EVENT>
async<int> stop_race_winner(EVENT& ready, bool fail) {
  co_await ready.wait();
#ifndef CORO_DISABLE_EXCEPTION
  if (fail) {
    throw std::runtime_error("stop race failed");
  }
#else
  (void)fail;
#endif
  co_return 42;
}

template <typename EVENT, typename GROUP>
async<void> await_cooperative_when_stop(bool fail) {
  EVENT stop;
  EVENT ready;
  GROUP finished;
  int started = 0;
  int cleaned = 0;
  finished.add(2);
#ifndef CORO_DISABLE_EXCEPTION
  bool caught = false;
  try {
#endif
    auto result = co_await when_any(stop_race_winner(ready, fail), when_all(stopped_when_branch(stop, ready, finished, started, cleaned),
                                                                            stopped_when_branch(stop, ready, finished, started, cleaned)));
    ASSERT(result.index == 0);
    ASSERT(result.template get<0>() == 42);
#ifndef CORO_DISABLE_EXCEPTION
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "stop race failed");
    caught = true;
  }
  ASSERT(caught == fail);
#endif
  ASSERT(started == 2);
  ASSERT(cleaned == 0);  // Neither success nor failure cancels the nested branches.
  stop.set();
  co_await finished.wait();
  ASSERT(cleaned == 2);
}

template <typename EVENT, typename GROUP>
void test_cooperative_when_stop(bool fail) {
  executor_poll executor;
  bool completed = false;
  await_cooperative_when_stop<EVENT, GROUP>(fail).detach_with_callback(executor, [&] {
    completed = true;
  });
  // Drain the nested combiner's completion after the branch cleanup notifications.
  for (int attempt = 0; attempt < 100; ++attempt) {
    executor.poll();
  }
  ASSERT(completed);
  check_coro_leak();
}

async<void> run_all_tests() {
  co_await test_when_all_values();
  co_await test_when_all_void();
  co_await test_when_any_values();
  co_await test_when_any_void();
  co_await test_when_all_mixed_types();
  co_await test_when_all_mixed_types2();
  co_await test_when_any_mixed_types();
  co_await test_when_all_immediate_completion();
  co_await test_when_all_empty_completion();
  co_await test_when_any_immediate_completion();
  co_await test_when_any_keeps_losers_running();
  LOG("=== ALL TESTS PASSED ===");

#ifndef CORO_DISABLE_EXCEPTION
  // Run exception tests (only when exception support is enabled)
  co_await test_when_all_exception();
  co_await test_when_all_void_exception();
  co_await test_when_all_mixed_exception();
  co_await test_when_any_exception_first();
  co_await test_when_any_exception_later();
  co_await test_when_any_void_exception();
  co_await test_when_all_multiple_exceptions();

  LOG("=== ALL EXCEPTION TESTS PASSED ===");
#else
  LOG("=== ALL EXCEPTION TESTS SKIPPED) ===");
#endif
}

int main() {
#ifndef CORO_SINGLE_THREAD
  test_when_all_result_publication(false);
#ifndef CORO_DISABLE_EXCEPTION
  test_when_all_result_publication(true);
#endif
#endif
  LOG("when_all/when_any test init");
#ifndef CORO_DISABLE_EXCEPTION
  test_rejected_when_launch<false>();
  test_rejected_when_launch<true>();
#endif
  test_nested_when<false, void>();
  test_nested_when<false, int>();
  test_nested_when<true, void>();
  test_nested_when<true, int>();
  test_when_all_reentrant_completion();
  test_when_any_reentrant_completion();
#ifndef CORO_SINGLE_THREAD
  test_when_any_result_publication();
#endif
  test_cooperative_when_stop<event_st, wait_group_st>(false);
  test_cooperative_when_stop<event_mt, wait_group_mt>(false);
#ifndef CORO_DISABLE_EXCEPTION
  test_cooperative_when_stop<event_st, wait_group_st>(true);
  test_cooperative_when_stop<event_mt, wait_group_mt>(true);
#endif
  executor_loop executor;
  run_all_tests().detach_with_callback(executor, [&] {
    executor.stop();
  });
  LOG("loop...");
  executor.run_loop();
  check_coro_leak();
  return 0;
}
