/// Test: coro and asio interoperability
/// This test demonstrates how coro::async and asio::awaitable can call each other

#define CORO_DEBUG_PROMISE_LEAK
#include <atomic>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

// coro
#include "coro.hpp"
#include "coro/adaptor/asio_adaptor.hpp"

// utils
#include "TimeCount.hpp"
#include "assert_def.h"
#include "log.h"
#include "utils.hpp"

using namespace coro;

/// ============================================================================
/// Part 1: Basic asio coroutines
/// ============================================================================

/// A simple asio coroutine that sleeps and returns a value
asio::awaitable<int> asio_sleep_and_return(int ms, int value) {
  auto executor = co_await asio::this_coro::executor;
  asio::steady_timer timer(executor);
  timer.expires_after(std::chrono::milliseconds(ms));
  co_await timer.async_wait(asio::use_awaitable);
  LOG("asio_sleep_and_return: slept %d ms, returning %d", ms, value);
  co_return value;
}

/// A simple asio coroutine that just sleeps (void return)
asio::awaitable<void> asio_sleep(int ms) {
  auto executor = co_await asio::this_coro::executor;
  asio::steady_timer timer(executor);
  timer.expires_after(std::chrono::milliseconds(ms));
  co_await timer.async_wait(asio::use_awaitable);
  LOG("asio_sleep: slept %d ms", ms);
}

/// ============================================================================
/// Part 2: Basic coro coroutines
/// ============================================================================

/// A simple coro coroutine that sleeps and returns a value
async<int> coro_sleep_and_return(int ms, int value) {
  co_await sleep(std::chrono::milliseconds(ms));
  LOG("coro_sleep_and_return: slept %d ms, returning %d", ms, value);
  co_return value;
}

/// A simple coro coroutine that just sleeps (void return)
async<void> coro_sleep(int ms) {
  co_await sleep(std::chrono::milliseconds(ms));
  LOG("coro_sleep: slept %d ms", ms);
}

/// ============================================================================
/// Part 3: coro calling asio (using await_asio)
/// ============================================================================

/// coro coroutine that calls asio coroutine
async<int> coro_calls_asio() {
  LOG("coro_calls_asio: start");

  // Call asio coroutine that returns int
  TimeCount t;
  int result = co_await await_asio(asio_sleep_and_return(100, 42));
  ASSERT(result == 42);
  ASSERT(t.elapsed() >= 100);
  LOG("coro_calls_asio: got result %d from asio", result);

  // Call asio coroutine that returns void
  t.reset();
  co_await await_asio(asio_sleep(50));
  ASSERT(t.elapsed() >= 50);
  LOG("coro_calls_asio: asio void call completed");

  LOG("coro_calls_asio: done");
  co_return result;
}

/// ============================================================================
/// Part 4: asio calling coro (using await_coro)
/// ============================================================================

/// asio coroutine that calls coro coroutine
asio::awaitable<int> asio_calls_coro() {
  LOG("asio_calls_coro: start");

  // Call coro coroutine that returns int
  TimeCount t;
  int result = co_await await_coro(coro_sleep_and_return(100, 99));
  ASSERT(result == 99);
  ASSERT(t.elapsed() >= 100);
  LOG("asio_calls_coro: got result %d from coro", result);

  // Call coro coroutine that returns void
  t.reset();
  co_await await_coro(coro_sleep(50));
  ASSERT(t.elapsed() >= 50);
  LOG("asio_calls_coro: coro void call completed");

  LOG("asio_calls_coro: done");
  co_return result;
}

/// ============================================================================
/// Part 5: Nested calls - coro -> asio -> coro
/// ============================================================================

/// asio coroutine that calls coro (to be called from coro)
asio::awaitable<int> asio_intermediate(int value) {
  LOG("asio_intermediate: received %d", value);

  // Call another coro coroutine from within asio
  int coro_result = co_await await_coro(coro_sleep_and_return(30, value * 2));
  LOG("asio_intermediate: coro returned %d", coro_result);

  co_return coro_result + 1;
}

/// coro coroutine that calls asio which calls coro (nested)
async<int> coro_nested_call() {
  LOG("coro_nested_call: start");

  // coro -> asio -> coro
  int result = co_await await_asio(asio_intermediate(10));
  ASSERT(result == 21);  // 10 * 2 + 1 = 21
  LOG("coro_nested_call: final result %d", result);

  co_return result;
}

/// ============================================================================
/// Part 6: Nested calls - asio -> coro -> asio
/// ============================================================================

/// coro coroutine that calls asio (to be called from asio)
async<int> coro_intermediate(int value) {
  LOG("coro_intermediate: received %d", value);

  // Call another asio coroutine from within coro
  int asio_result = co_await await_asio(asio_sleep_and_return(30, value * 3));
  LOG("coro_intermediate: asio returned %d", asio_result);

  co_return asio_result + 2;
}

/// asio coroutine that calls coro which calls asio (nested)
asio::awaitable<int> asio_nested_call() {
  LOG("asio_nested_call: start");

  // asio -> coro -> asio
  int result = co_await await_coro(coro_intermediate(5));
  ASSERT(result == 17);  // 5 * 3 + 2 = 17
  LOG("asio_nested_call: final result %d", result);

  co_return result;
}

/// ============================================================================
/// Part 7: Exception handling
/// ============================================================================

#ifndef CORO_DISABLE_EXCEPTION
async<int> coro_throws() {
  co_await sleep(10ms);
  throw std::runtime_error("coro exception");
  co_return 0;
}

asio::awaitable<int> asio_throws() {
  auto executor = co_await asio::this_coro::executor;
  asio::steady_timer timer(executor);
  timer.expires_after(std::chrono::milliseconds(10));
  co_await timer.async_wait(asio::use_awaitable);
  throw std::runtime_error("asio exception");
  co_return 0;
}

/// Test exception propagation from asio to coro
async<void> test_asio_exception_in_coro() {
  LOG("test_asio_exception_in_coro: start");
  bool caught = false;
  try {
    co_await await_asio(asio_throws());
  } catch (const std::runtime_error& e) {
    LOG("test_asio_exception_in_coro: caught exception: %s", e.what());
    ASSERT(std::string_view(e.what()) == "asio exception");
    caught = true;
  }
  ASSERT(caught);
  LOG("test_asio_exception_in_coro: done");
}

/// Test exception propagation from coro to asio
asio::awaitable<void> test_coro_exception_in_asio() {
  LOG("test_coro_exception_in_asio: start");
  bool caught = false;
  try {
    co_await await_coro(coro_throws());
  } catch (const std::runtime_error& e) {
    LOG("test_coro_exception_in_asio: caught exception: %s", e.what());
    ASSERT(std::string_view(e.what()) == "coro exception");
    caught = true;
  }
  ASSERT(caught);
  LOG("test_coro_exception_in_asio: done");
}
#endif

/// ============================================================================
/// Part 8: Concurrent execution
/// ============================================================================

/// Test running multiple cross-framework coroutines concurrently
async<void> test_concurrent() {
  LOG("test_concurrent: start");

  std::atomic<int> counter{0};

  // Await every child before releasing the counter they share.
  LOG("test_concurrent: counter = %d", counter.load());
  auto child = [](std::atomic<int>& cnt, int milliseconds, int index) -> async<void> {
    co_await await_asio(asio_sleep(milliseconds));
    cnt++;
    LOG("test_concurrent: task %d, counter = %d", index, cnt.load());
  };
  co_await when_all(child(counter, 50, 1), child(counter, 60, 2), child(counter, 70, 3));

  LOG("test_concurrent: done, counter = %d", counter.load());
  ASSERT(counter == 3);
}

void test_concurrent_with_stalled_loop() {
  asio::io_context context;
  asio_executor exec(context);
  bool completed = false;
  test_concurrent().detach(exec, [&] {
    completed = true;
  });
  asio::steady_timer stall(context);
  stall.expires_after(10ms);
  stall.async_wait([](asio::error_code error) {
    ASSERT(!error);
    // Let all timers expire before the adaptor's posted completions can run.
    std::this_thread::sleep_for(250ms);
  });
  context.run();
  ASSERT(completed);
  check_coro_leak();
}

async<int> immediate_coro_value([[maybe_unused]] bool failure) {
#ifndef CORO_DISABLE_EXCEPTION
  if (failure) {
    throw std::runtime_error("immediate coro failure");
  }
#endif
  co_return 42;
}

async<void> immediate_coro_void([[maybe_unused]] bool failure) {
#ifndef CORO_DISABLE_EXCEPTION
  if (failure) {
    throw std::runtime_error("immediate coro failure");
  }
#endif
  co_return;
}

asio::awaitable<void> await_immediate_coro(bool void_result, bool failure, bool& completed) {
#ifndef CORO_DISABLE_EXCEPTION
  try {
#endif
    if (void_result) {
      co_await await_coro(immediate_coro_void(failure));
    } else {
      int result = co_await await_coro(immediate_coro_value(failure));
      ASSERT(result == 42);
    }
    ASSERT(!failure);
#ifndef CORO_DISABLE_EXCEPTION
  } catch (const std::runtime_error& error) {
    ASSERT(failure);
    ASSERT(std::string_view(error.what()) == "immediate coro failure");
  }
#endif
  completed = true;
}

void test_immediate_coro_completion(bool void_result, bool failure = false) {
  asio::io_context context;
  bool completed = false;
  asio::co_spawn(context, await_immediate_coro(void_result, failure, completed), [](std::exception_ptr exception) {
    ASSERT(!exception);
  });
  context.run_for(100ms);
  ASSERT(context.stopped());
  ASSERT(completed);
  ASSERT(debug_coro_promise::debug_coro_leak.empty());
}

struct nonassignable_value {
  int value;

  explicit nonassignable_value(int v) : value(v) {}
  nonassignable_value(nonassignable_value&&) noexcept = default;
  nonassignable_value(const nonassignable_value&) = delete;
  nonassignable_value& operator=(nonassignable_value&&) = delete;
  nonassignable_value& operator=(const nonassignable_value&) = delete;
};

async<nonassignable_value> make_coro_nonassignable_value(bool delayed) {
  if (delayed) {
    co_await sleep(1ms);
  }
  co_return nonassignable_value{42};
}

asio::awaitable<nonassignable_value> make_asio_nonassignable_value(bool delayed, [[maybe_unused]] bool fail = false) {
  if (delayed) {
    co_await asio_sleep(1);
  }
#ifndef CORO_DISABLE_EXCEPTION
  if (fail) {
    throw std::runtime_error("nondefault value failure");
  }
#endif
  co_return nonassignable_value{53};
}

async<void> test_nonassignable_asio_values() {
  for (bool delayed : {false, true}) {
    auto value = co_await await_asio(make_asio_nonassignable_value(delayed));
    ASSERT(value.value == 53);
#ifndef CORO_DISABLE_EXCEPTION
    bool failed = false;
    try {
      co_await await_asio(make_asio_nonassignable_value(delayed, true));
    } catch (const std::runtime_error& error) {
      ASSERT(std::string_view(error.what()) == "nondefault value failure");
      failed = true;
    }
    ASSERT(failed);
#endif
  }
}

asio::awaitable<void> test_nonassignable_coro_values() {
  for (bool delayed : {false, true}) {
    auto value = co_await await_coro(make_coro_nonassignable_value(delayed));
    ASSERT(value.value == 42);
  }
}

async<int> coro_threaded_value([[maybe_unused]] bool fail = false) {
  co_await sleep(0ns);
#ifndef CORO_DISABLE_EXCEPTION
  if (fail) {
    throw std::runtime_error("threaded value failure");
  }
#endif
  co_return 42;
}

async<void> coro_threaded_void([[maybe_unused]] bool fail = false) {
  co_await sleep(0ns);
#ifndef CORO_DISABLE_EXCEPTION
  if (fail) {
    throw std::runtime_error("threaded void failure");
  }
#endif
}

asio::awaitable<void> await_threaded_coro(std::atomic<int>& completed) {
  for (int iteration = 0; iteration < 128; ++iteration) {
    int value = co_await await_coro(coro_threaded_value());
    ASSERT(value == 42);
    co_await await_coro(coro_threaded_void());
#ifndef CORO_DISABLE_EXCEPTION
    bool value_failed = false;
    try {
      co_await await_coro(coro_threaded_value(true));
    } catch (const std::runtime_error& error) {
      ASSERT(std::string_view(error.what()) == "threaded value failure");
      value_failed = true;
    }
    ASSERT(value_failed);
    bool void_failed = false;
    try {
      co_await await_coro(coro_threaded_void(true));
    } catch (const std::runtime_error& error) {
      ASSERT(std::string_view(error.what()) == "threaded void failure");
      void_failed = true;
    }
    ASSERT(void_failed);
#endif
  }
  completed.fetch_add(1, std::memory_order_relaxed);
}

void test_multithreaded_await_coro() {
  asio::io_context context;
  std::atomic<int> completed{0};
  for (int task = 0; task < 16; ++task) {
    asio::co_spawn(context, await_threaded_coro(completed), [](std::exception_ptr exception) {
      ASSERT(!exception);
    });
  }
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&context] {
      context.run_for(5s);
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  ASSERT(context.stopped());
  ASSERT(completed.load(std::memory_order_relaxed) == 16);
  ASSERT(debug_coro_promise::debug_coro_leak.empty());
}

void test_large_asio_delayed_post() {
  for (uint64_t delay : {std::numeric_limits<uint64_t>::max(), static_cast<uint64_t>(std::chrono::nanoseconds::max().count())}) {
    asio::io_context context;
    asio_executor exec(context);
    bool called = false;
    exec.post_delayed_ns(
        [&called] {
          called = true;
        },
        delay);
    context.run_for(1ms);
    ASSERT(!called);
  }
}

using test_strand = asio::strand<asio::io_context::executor_type>;

asio::awaitable<int> asio_strand_value(test_strand& strand, [[maybe_unused]] bool fail = false) {
  ASSERT(strand.running_in_this_thread());
  auto exec = co_await asio::this_coro::executor;
  asio::steady_timer timer(exec);
  timer.expires_after(0ns);
  co_await timer.async_wait(asio::use_awaitable);
  ASSERT(strand.running_in_this_thread());
#ifndef CORO_DISABLE_EXCEPTION
  if (fail) {
    throw std::runtime_error("strand asio failure");
  }
#endif
  co_return 42;
}

asio::awaitable<void> asio_strand_void(test_strand& strand, bool fail = false) {
  ASSERT(strand.running_in_this_thread());
  int value = co_await asio_strand_value(strand, fail);
  ASSERT(value == 42);
}

async<int> coro_strand_value(test_strand& strand, bool nested, [[maybe_unused]] bool fail) {
  ASSERT(strand.running_in_this_thread());
  co_await sleep(0ns);
  ASSERT(strand.running_in_this_thread());
  if (nested) {
    int value = co_await await_asio(asio_strand_value(strand));
    ASSERT(value == 42);
    ASSERT(strand.running_in_this_thread());
    co_await await_asio(asio_strand_void(strand));
    ASSERT(strand.running_in_this_thread());
#ifndef CORO_DISABLE_EXCEPTION
    for (bool void_result : {false, true}) {
      bool caught = false;
      try {
        if (void_result) {
          co_await await_asio(asio_strand_void(strand, true));
        } else {
          co_await await_asio(asio_strand_value(strand, true));
        }
      } catch (const std::runtime_error& error) {
        ASSERT(std::string_view(error.what()) == "strand asio failure");
        caught = true;
      }
      ASSERT(caught);
      ASSERT(strand.running_in_this_thread());
    }
#endif
  }
#ifndef CORO_DISABLE_EXCEPTION
  if (fail) {
    throw std::runtime_error("strand task failure");
  }
#endif
  co_return 42;
}

async<void> coro_strand_void(test_strand& strand, bool nested, bool fail) {
  int value = co_await coro_strand_value(strand, nested, fail);
  ASSERT(value == 42);
  ASSERT(strand.running_in_this_thread());
}

asio::awaitable<void> await_strand_coro(test_strand& strand, std::atomic<int>& completed) {
  for (bool nested : {false, true}) {
#ifndef CORO_DISABLE_EXCEPTION
    for (bool fail : {false, true}) {
#else
    for (bool fail : {false}) {
#endif
      for (bool void_result : {false, true}) {
        ASSERT(strand.running_in_this_thread());
        bool caught = false;
#ifndef CORO_DISABLE_EXCEPTION
        try {
#endif
          if (void_result) {
            co_await await_coro(coro_strand_void(strand, nested, fail));
          } else {
            int value = co_await await_coro(coro_strand_value(strand, nested, fail));
            ASSERT(value == 42);
          }
#ifndef CORO_DISABLE_EXCEPTION
        } catch (const std::runtime_error& error) {
          ASSERT(std::string_view(error.what()) == "strand task failure");
          caught = true;
        }
#endif
        ASSERT(caught == fail);
        ASSERT(strand.running_in_this_thread());
      }
    }
  }
  completed.fetch_add(1, std::memory_order_relaxed);
}

void test_strand_interoperability(int worker_count) {
  asio::io_context context;
  auto strand = asio::make_strand(context);
  std::atomic<int> completed{0};
  for (int task = 0; task < 16; ++task) {
    asio::co_spawn(strand, await_strand_coro(strand, completed), [](std::exception_ptr exception) {
      ASSERT(!exception);
    });
  }
  std::vector<std::thread> workers;
  for (int worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&context] {
      context.run_for(5s);
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  ASSERT(context.stopped());
  ASSERT(completed.load(std::memory_order_relaxed) == 16);
  ASSERT(debug_coro_promise::debug_coro_leak.empty());
}

template <typename T>
concept has_stop = requires(T& value) { value.stop(); };

template <typename T>
concept has_io_context = requires(T& value) { value.get_io_context(); };

static_assert(!has_stop<executor>);
static_assert(!has_stop<asio_executor>);
static_assert(!has_io_context<asio_executor>);
static_assert(has_stop<executor_loop>);
static_assert(has_stop<executor_poll>);
static_assert(std::is_constructible_v<asio_executor, asio::any_io_executor>);

async<void> publicly_bound_child(asio_executor& exec, asio::io_context& context, test_strand* strand) {
  auto check_executor = [&exec, &context, strand](executor* inherited) {
    ASSERT(inherited == &exec);
    ASSERT(context.get_executor().running_in_this_thread());
    ASSERT(!strand || strand->running_in_this_thread());
  };
  for (bool delayed : {false, true}) {
    co_await callback_awaiter<void>([check_executor, delayed](executor* inherited, auto callback) {
      check_executor(inherited);
      if (delayed) {
        inherited->post(std::move(callback));
      } else {
        callback();
      }
    });
    check_executor(&exec);
    co_await callback_awaiter<void>([&exec, delayed](auto callback) {
      if (delayed) {
        exec.post(std::move(callback));
      } else {
        callback();
      }
    });
    check_executor(&exec);
    auto value = co_await await_asio(make_asio_nonassignable_value(delayed));
    ASSERT(value.value == 53);
    check_executor(&exec);
  }
  co_await sleep(0ns);
  check_executor(&exec);
  if (strand) {
    co_await coro_strand_void(*strand, true, false);
    check_executor(&exec);
    // A child on the plain executor must return its parent to the strand.
    asio_executor other(context.get_executor());
    auto value = co_await make_coro_nonassignable_value(true).bind_executor(other);
    ASSERT(value.value == 42);
    check_executor(&exec);
  }
}

async<void> publicly_bound_parent(asio_executor& exec, asio::io_context& context, test_strand* strand, std::atomic<int>& completed) {
  co_await publicly_bound_child(exec, context, strand);
  completed.fetch_add(1, std::memory_order_relaxed);
}

void test_public_executor_binding(bool use_strand, int worker_count) {
  asio::io_context context;
  auto strand = asio::make_strand(context);
  asio_executor plain_exec(asio::any_io_executor{context.get_executor()});
  asio_executor strand_exec([&strand] {
    asio::ip::tcp::socket socket(strand);
    return socket.get_executor();
  }());
  auto& exec = use_strand ? strand_exec : plain_exec;
  if (use_strand) {
    ASSERT(exec.get_asio_executor() == asio::any_io_executor(strand));
  } else {
    ASSERT(exec.get_asio_executor() == asio::any_io_executor(context.get_executor()));
  }
  std::atomic<int> completed{0};
  for (int task = 0; task < 16; ++task) {
    auto child = publicly_bound_parent(exec, context, use_strand ? &strand : nullptr, completed);
    if (task % 2 == 0) {
      // Explicit binding takes precedence over the executor supplied to spawn.
      spawn(use_strand ? plain_exec : strand_exec, child.bind_executor(exec));
    } else {
      spawn(exec, std::move(child));
    }
  }
  std::vector<std::thread> workers;
  for (int worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&context] {
      context.run_for(5s);
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  ASSERT(context.stopped());
  ASSERT(completed.load(std::memory_order_relaxed) == 16);
  check_coro_leak();
}

async<void> stop_asio_context(asio::io_context& context, bool& started, bool& completed, std::function<void()>& release) {
  started = true;
  context.stop();
  co_await callback_awaiter<void>([&release](auto callback) {
    release = std::move(callback);
  });
  completed = true;
}

void test_caller_stops_context(bool use_strand) {
  asio::io_context context;
  auto work = asio::make_work_guard(context);
  asio_executor plain_exec(context.get_executor());
  asio_executor strand_exec(asio::make_strand(context));
  auto& exec = use_strand ? strand_exec : plain_exec;
  bool started = false;
  bool completed = false;
  bool unrelated_completed = false;
  std::function<void()> release;
  spawn(exec, stop_asio_context(context, started, completed, release));
  context.run();
  ASSERT(context.stopped());
  ASSERT(started);
  ASSERT(!completed);
  ASSERT(release);
  // stop() is not a barrier for ready handlers on every backend. Queue work after run() returns.
  asio::post(context, [&unrelated_completed] {
    unrelated_completed = true;
  });
  release();
  release = nullptr;
  ASSERT(!completed);
  ASSERT(!unrelated_completed);
  context.restart();
  work.reset();
  context.run();
  ASSERT(completed);
  ASSERT(unrelated_completed);
  check_coro_leak();
}

async<void> pool_child() {
  for (bool delayed : {false, true}) {
    auto value = co_await await_asio(make_asio_nonassignable_value(delayed));
    ASSERT(value.value == 53);
  }
  co_await await_asio(asio_sleep(0));
}

asio::awaitable<void> await_pool_coro(std::atomic<int>& completed) {
  for (bool delayed : {false, true}) {
    auto value = co_await await_coro(make_coro_nonassignable_value(delayed));
    ASSERT(value.value == 42);
  }
  co_await await_coro(pool_child());
  completed.fetch_add(1, std::memory_order_relaxed);
}

void test_thread_pool_executor(int worker_count) {
  asio::thread_pool pool(worker_count);
  asio_executor exec(asio::any_io_executor{pool.get_executor()});
  std::atomic<int> completed{0};
  pool_child().detach(exec, [&completed] {
    completed.fetch_add(1, std::memory_order_relaxed);
  });
  asio::co_spawn(pool, await_pool_coro(completed), [](std::exception_ptr error) {
    ASSERT(!error);
  });
  pool.join();
  ASSERT(completed.load(std::memory_order_relaxed) == 2);
  check_coro_leak();
}

#ifndef CORO_DISABLE_EXCEPTION
template <typename T>
async<T> cancellable_wait_child(std::function<void()>& release, bool& completed) {
  co_await callback_awaiter<void>([&release](auto callback) {
    release = std::move(callback);
  });
  completed = true;
  if constexpr (!std::is_void_v<T>) {
    co_return 42;
  }
}

template <typename T>
asio::awaitable<void> await_cancelled_child(std::function<void()>& release, bool& completed) {
  co_await await_coro(cancellable_wait_child<T>(release, completed));
}

template <typename T>
void test_cancelled_await_coro() {
  asio::io_context context;
  asio::cancellation_signal cancellation;
  std::function<void()> release;
  bool child_completed = false;
  bool parent_completed = false;
  std::exception_ptr exception;
  asio::co_spawn(context, await_cancelled_child<T>(release, child_completed),
                 asio::bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr error) {
                   exception = error;
                   parent_completed = true;
                 }));
  context.poll();
  ASSERT(release);
  ASSERT(!parent_completed);
  cancellation.emit(asio::cancellation_type::terminal);
  context.poll();
  bool cancelled_before_child_completed = parent_completed && !child_completed;

  // Cancelling the ASIO wait does not cancel the detached coro task.
  release();
  release = nullptr;
  context.restart();
  context.run_for(1s);
  ASSERT(context.stopped());
  ASSERT(child_completed);
  ASSERT(cancelled_before_child_completed);
  bool cancelled = false;
  if (exception) {
    try {
      std::rethrow_exception(exception);
    } catch (const asio::system_error& error) {
      cancelled = error.code() == asio::error::operation_aborted;
    } catch (...) {
    }
  }
  ASSERT(cancelled);
  ASSERT(debug_coro_promise::debug_coro_leak.empty());
}
#endif

/// ============================================================================
/// Main test runner
/// ============================================================================

struct asio_result_lifetime_state {
  std::atomic<int> live{0};
  std::atomic<bool> fail_move{false};
  std::atomic<bool> parent_completed{false};
  int retained_after_await = -1;
  bool caught = false;
};

struct asio_lifetime_value {
  asio_result_lifetime_state* state;

  explicit asio_lifetime_value(asio_result_lifetime_state& value_state) : state(&value_state) {
    ++state->live;
  }
  asio_lifetime_value(const asio_lifetime_value&) = delete;
  asio_lifetime_value(asio_lifetime_value&& other) : state(other.state) {
    if (state->fail_move.load()) {
      throw std::runtime_error("ASIO result extraction failed");
    }
    ++state->live;
  }
  ~asio_lifetime_value() {
    --state->live;
  }
};

struct retaining_post_executor : asio_executor {
  asio_result_lifetime_state& state;
  bool fail_extraction;

  retaining_post_executor(asio::io_context& context, asio_result_lifetime_state& value_state, bool fail)
      : asio_executor(context), state(value_state), fail_extraction(fail) {}

  void post(std::function<void()> fn) override {
    state.fail_move.store(fail_extraction);
    asio_executor::post(std::move(fn));
    // Keep the completion callback alive while another worker resumes the parent.
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!state.parent_completed.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    ASSERT(state.parent_completed.load());
  }
};

asio::awaitable<asio_lifetime_value> make_asio_lifetime_value(asio_result_lifetime_state& state, bool delayed) {
  if (delayed) {
    co_await asio::post(asio::use_awaitable);
  }
  co_return asio_lifetime_value{state};
}

async<void> await_asio_result_lifetime(asio_result_lifetime_state& state, bool delayed) {
  try {
    auto result = co_await await_asio(make_asio_lifetime_value(state, delayed));
    ASSERT(result.state == &state);
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "ASIO result extraction failed");
    state.caught = true;
  }
  state.retained_after_await = state.live.load();
  state.parent_completed.store(true);
}

void test_asio_result_lifetime(bool delayed, bool fail) {
  asio::io_context context;
  asio_result_lifetime_state state;
  retaining_post_executor executor(context, state, fail);
  spawn(executor, await_asio_result_lifetime(state, delayed));
  std::thread worker([&context] {
    context.run_for(3s);
  });
  context.run_for(3s);
  worker.join();
  ASSERT(context.stopped());
  ASSERT(state.parent_completed.load());
  ASSERT(state.caught == fail);
  ASSERT(state.live.load() == 0);
  ASSERT(state.retained_after_await == 0);
  check_coro_leak();
}

struct asio_lifetime_error : std::exception {
  asio_result_lifetime_state* state;

  explicit asio_lifetime_error(asio_result_lifetime_state& value_state) : state(&value_state) {
    ++state->live;
  }
  asio_lifetime_error(const asio_lifetime_error& other) noexcept : state(other.state) {
    ++state->live;
  }
  ~asio_lifetime_error() override {
    --state->live;
  }
};

template <typename T>
asio::awaitable<T> throw_asio_lifetime_error(asio_result_lifetime_state& state, bool delayed) {
  if (delayed) {
    co_await asio::post(asio::use_awaitable);
  }
  throw asio_lifetime_error{state};
  if constexpr (!std::is_void_v<T>) {
    co_return 42;
  }
}

template <typename T>
async<void> await_asio_exception_lifetime(asio_result_lifetime_state& state, bool delayed) {
  try {
    co_await await_asio(throw_asio_lifetime_error<T>(state, delayed));
  } catch (const asio_lifetime_error& error) {
    ASSERT(error.state == &state);
    state.caught = true;
  }
  state.retained_after_await = state.live.load();
  state.parent_completed.store(true);
}

template <typename T>
void test_asio_exception_lifetime(bool delayed) {
  asio::io_context context;
  asio_result_lifetime_state state;
  retaining_post_executor executor(context, state, false);
  spawn(executor, await_asio_exception_lifetime<T>(state, delayed));
  std::thread worker([&context] {
    context.run_for(3s);
  });
  context.run_for(3s);
  worker.join();
  ASSERT(context.stopped());
  ASSERT(state.parent_completed.load());
  ASSERT(state.caught);
  ASSERT(state.live.load() == 0);
  ASSERT(state.retained_after_await == 0);
  check_coro_leak();
}

struct coro_result_lifetime_state {
  std::atomic<int> live{0};
  std::atomic<bool> parent_completed{false};
  std::thread::id producer_thread;
  bool delayed = false;
  int fail_at_move = 0;
  int moves = 0;
  bool move_failed = false;
  bool caught = false;
  int retained_after_await = -1;
};

struct coro_lifetime_value {
  coro_result_lifetime_state* state;

  explicit coro_lifetime_value(coro_result_lifetime_state& value_state) : state(&value_state) {
    ++state->live;
  }
  coro_lifetime_value(const coro_lifetime_value&) = delete;
  coro_lifetime_value(coro_lifetime_value&& other) : state(other.state) {
    if (++state->moves == state->fail_at_move) {
      state->move_failed = true;
      throw std::runtime_error("coro result move failed");
    }
    ++state->live;
  }
  ~coro_lifetime_value() {
    if (state->delayed && state->fail_at_move == 0 && std::this_thread::get_id() == state->producer_thread && state->live.load() > 2) {
      // Keep extra producer-side temporaries alive while the other worker receives the result.
      auto deadline = std::chrono::steady_clock::now() + 2s;
      while (!state->parent_completed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
      }
    }
    --state->live;
  }
};

async<coro_lifetime_value> make_coro_lifetime_value(coro_result_lifetime_state& state) {
  if (state.delayed) {
    co_await sleep(0ns);
  }
  state.producer_thread = std::this_thread::get_id();
  co_return coro_lifetime_value{state};
}

asio::awaitable<void> await_coro_result_lifetime(coro_result_lifetime_state& state) {
  try {
    auto result = co_await await_coro(make_coro_lifetime_value(state));
    ASSERT(result.state == &state);
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "coro result move failed");
    state.caught = true;
  }
  state.retained_after_await = state.live.load();
  state.parent_completed.store(true);
}

void test_coro_result_lifetime(bool delayed, int fail_at_move) {
  asio::io_context context;
  coro_result_lifetime_state state;
  state.delayed = delayed;
  state.fail_at_move = fail_at_move;
  asio::co_spawn(context, await_coro_result_lifetime(state), [](std::exception_ptr error) {
    ASSERT(!error);
  });
  std::thread worker([&context] {
    context.run_for(5s);
  });
  context.run_for(5s);
  worker.join();
  ASSERT(context.stopped());
  ASSERT(state.parent_completed.load());
  ASSERT(state.caught == state.move_failed);
  ASSERT(state.live.load() == 0);
  ASSERT(state.retained_after_await == 0);
  check_coro_leak();
}

template <typename T>
asio::awaitable<T> asio_stops_before_completion(asio::io_context& context, bool fail) {
  co_await asio::post(asio::use_awaitable);
  context.stop();
  if (fail) {
    throw std::runtime_error("completion after stop");
  }
  if constexpr (!std::is_void_v<T>) {
    co_return 42;
  }
}

template <typename T>
async<void> await_asio_across_restart(asio::io_context& context, bool fail, bool& completed, bool& caught) {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await await_asio(asio_stops_before_completion<T>(context, fail));
    } else {
      ASSERT(co_await await_asio(asio_stops_before_completion<T>(context, fail)) == 42);
    }
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "completion after stop");
    caught = true;
  }
  completed = true;
}

template <typename T>
void test_await_asio_survives_restart(bool fail) {
  asio::io_context context;
  asio_executor executor(context);
  bool completed = false;
  bool caught = false;
  spawn(executor, await_asio_across_restart<T>(context, fail, completed, caught));
  context.run();
  ASSERT(context.stopped());
  ASSERT(!completed);
  context.restart();
  context.run();
  ASSERT(completed);
  ASSERT(caught == fail);
  check_coro_leak();
}

async<void> run_all_tests() {
  LOG("=== Starting all tests ===");
  co_await test_nonassignable_asio_values();

  // Test 1: coro calling asio
  {
    LOG("--- Test 1: coro calling asio ---");
    TimeCount t;
    int result = co_await coro_calls_asio();
    ASSERT(result == 42);
    LOG("Test 1 passed in %d ms", (int)t.elapsed());
  }

  // Test 2: Nested coro -> asio -> coro
  {
    LOG("--- Test 2: Nested coro -> asio -> coro ---");
    TimeCount t;
    int result = co_await coro_nested_call();
    ASSERT(result == 21);
    LOG("Test 2 passed in %d ms", (int)t.elapsed());
  }

#ifndef CORO_DISABLE_EXCEPTION
  // Test 3: Exception from asio in coro
  {
    LOG("--- Test 3: Exception from asio in coro ---");
    co_await test_asio_exception_in_coro();
    LOG("Test 3 passed");
  }
#endif

  // Test 4: Concurrent execution
  {
    LOG("--- Test 4: Concurrent execution ---");
    co_await test_concurrent();
    LOG("Test 4 passed");
  }

  LOG("=== All coro-side tests passed ===");
}

// Exercise concrete io_context dispatch and the strand/property-preserving fallback.
void test_executor_submission(int kind) {
  asio::io_context context;
  int calls = 0;
  {
    asio::any_io_executor selected = context.get_executor();
    if (kind == 1) {
      selected = asio::make_strand(context);
    } else if (kind == 2) {
      selected = asio::prefer(context.get_executor(), asio::execution::outstanding_work.tracked);
    }
    asio_executor exec(selected);
    ASSERT(exec.get_asio_executor() == selected);
    int resumed = 0;
    auto probe = [](asio::io_context& context, int& resumed) -> async<void> {
      ASSERT(context.get_executor().running_in_this_thread());
      ++resumed;
      co_return;
    };
    auto queued_probe = probe(context, resumed).bind_executor(exec);
    auto inline_probe = probe(context, resumed).bind_executor(exec);
    exec.dispatch(detail::resume_task{queued_probe.current_coro_handle_});
    ASSERT(resumed == 0);
    exec.dispatch([&] {
      ASSERT(resumed == 1);
      exec.dispatch(detail::resume_task{inline_probe.current_coro_handle_});
      ASSERT(resumed == 2);
      ASSERT(calls == 0);
      exec.dispatch([&] {
        ++calls;
      });
      ASSERT(calls == 1);
      exec.post([&] {
        ASSERT(calls == 2);
        ++calls;
      });
      ASSERT(calls == 1);
      ++calls;
    });
    ASSERT(calls == 0);
    context.poll();
    ASSERT(calls == 3);
    ASSERT(resumed == 2);
    if (kind == 2) {
      ASSERT(!context.stopped());
    }
  }
  context.run();
  ASSERT(context.stopped());
  check_coro_leak();
}

int main() {
  for (int kind = 0; kind < 3; ++kind) {
    test_executor_submission(kind);
  }
  test_concurrent_with_stalled_loop();
#ifndef CORO_DISABLE_EXCEPTION
  for (bool delayed : {false, true}) {
    for (bool fail : {false, true}) {
      test_asio_result_lifetime(delayed, fail);
    }
    test_asio_exception_lifetime<int>(delayed);
    test_asio_exception_lifetime<void>(delayed);
  }
  for (bool delayed : {false, true}) {
    for (int fail_at_move = 0; fail_at_move <= 8; ++fail_at_move) {
      test_coro_result_lifetime(delayed, fail_at_move);
    }
  }
  test_await_asio_survives_restart<int>(false);
  test_await_asio_survives_restart<void>(false);
  test_await_asio_survives_restart<int>(true);
  test_await_asio_survives_restart<void>(true);
  test_cancelled_await_coro<void>();
  test_cancelled_await_coro<int>();
#endif
  test_strand_interoperability(1);
  test_strand_interoperability(4);
  for (bool use_strand : {false, true}) {
    test_public_executor_binding(use_strand, 1);
    test_public_executor_binding(use_strand, 4);
    test_caller_stops_context(use_strand);
  }
  test_thread_pool_executor(1);
  test_thread_pool_executor(4);
  test_large_asio_delayed_post();
  LOG("=== coro-asio adaptor test ===");
  test_multithreaded_await_coro();
  test_immediate_coro_completion(false);
  test_immediate_coro_completion(true);
#ifndef CORO_DISABLE_EXCEPTION
  test_immediate_coro_completion(false, true);
  test_immediate_coro_completion(true, true);
#endif

  asio::io_context io_ctx;

  // Create asio_executor that wraps io_context
  asio_executor exec(io_ctx);

  // Track test completion
  std::atomic<int> tests_completed{0};
  constexpr int CORO_TESTS = 1;  // run_all_tests
#ifndef CORO_DISABLE_EXCEPTION
  constexpr int ASIO_TESTS = 3;  // A1, A2, A3
#else
  constexpr int ASIO_TESTS = 2;  // A1, A2 (no exception tests)
#endif
  constexpr int TOTAL_TESTS = CORO_TESTS + ASIO_TESTS;

  // Run all coro-side tests
  spawn(exec, [](std::atomic<int>& tests_completed) -> async<void> {
    co_await run_all_tests();
    tests_completed++;
    LOG("Coro-side tests completed (%d/%d)", tests_completed.load(), TOTAL_TESTS);
  }(tests_completed));

  // Run all asio-side tests
  // Test A1: asio calling coro
  asio::co_spawn(
      io_ctx,
      [&tests_completed]() -> asio::awaitable<void> {
        co_await test_nonassignable_coro_values();
        LOG("--- Test A1: asio calling coro ---");
        TimeCount t;
        int result = co_await asio_calls_coro();
        ASSERT(result == 99);
        LOG("Test A1 passed in %d ms", (int)t.elapsed());
        tests_completed++;
        LOG("Test A1 completed (%d/%d)", tests_completed.load(), TOTAL_TESTS);
      },
      [](const std::exception_ptr& ep) {
        if (ep) {
          try {
            std::rethrow_exception(ep);
          } catch (const std::exception& e) {
            LOG("Test A1 failed with exception: %s", e.what());
            ASSERT(false);
          }
        }
      });

  // Test A2: Nested asio -> coro -> asio
  asio::co_spawn(
      io_ctx,
      [&tests_completed]() -> asio::awaitable<void> {
        LOG("--- Test A2: Nested asio -> coro -> asio ---");
        TimeCount t;
        int result = co_await asio_nested_call();
        ASSERT(result == 17);
        LOG("Test A2 passed in %d ms", (int)t.elapsed());
        tests_completed++;
        LOG("Test A2 completed (%d/%d)", tests_completed.load(), TOTAL_TESTS);
      },
      [](const std::exception_ptr& ep) {
        if (ep) {
          try {
            std::rethrow_exception(ep);
          } catch (const std::exception& e) {
            LOG("Test A2 failed with exception: %s", e.what());
            ASSERT(false);
          }
        }
      });

#ifndef CORO_DISABLE_EXCEPTION
  // Test A3: Exception from coro in asio
  asio::co_spawn(
      io_ctx,
      [&tests_completed]() -> asio::awaitable<void> {
        LOG("--- Test A3: Exception from coro in asio ---");
        co_await test_coro_exception_in_asio();
        LOG("Test A3 passed");
        tests_completed++;
        LOG("Test A3 completed (%d/%d)", tests_completed.load(), TOTAL_TESTS);
      },
      [](const std::exception_ptr& ep) {
        if (ep) {
          try {
            std::rethrow_exception(ep);
          } catch (const std::exception& e) {
            LOG("Test A3 failed with exception: %s", e.what());
            ASSERT(false);
          }
        }
      });
#endif

  LOG("Running io_context...");
  io_ctx.run_for(5s);
  ASSERT(io_ctx.stopped());

  // Verify all tests completed
  if (tests_completed == TOTAL_TESTS) {
    LOG("=== All %d tests passed! ===", TOTAL_TESTS);
  } else {
    LOG("=== ERROR: Only %d/%d tests completed ===", tests_completed.load(), TOTAL_TESTS);
    return 1;
  }

  // Check for coroutine leaks
  check_coro_leak();

  return 0;
}
