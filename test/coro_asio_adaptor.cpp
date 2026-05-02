/// Test: coro and asio interoperability
/// This test demonstrates how coro::async and asio::awaitable can call each other

#define CORO_DEBUG_PROMISE_LEAK
#include <atomic>
#include <limits>
#include <thread>
#include <vector>

// coro
#include "coro.hpp"
#include "coro/adaptor/asio_adaptor.hpp"

// utils
#include "TimeCount.hpp"
#include "assert_def.h"
#include "log.h"

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

  // Spawn multiple coro coroutines that call asio
  auto* exec = co_await current_executor();
  LOG("test_concurrent: counter = %d", counter.load());

  spawn(*exec, [](std::atomic<int>& cnt) -> async<void> {
    co_await await_asio(asio_sleep(50));
    cnt++;
    LOG("test_concurrent: task 1, counter = %d", cnt.load());
  }(counter));

  spawn(*exec, [](std::atomic<int>& cnt) -> async<void> {
    co_await await_asio(asio_sleep(60));
    cnt++;
    LOG("test_concurrent: task 2, counter = %d", cnt.load());
  }(counter));

  spawn(*exec, [](std::atomic<int>& cnt) -> async<void> {
    co_await await_asio(asio_sleep(70));
    cnt++;
    LOG("test_concurrent: task 3, counter = %d", cnt.load());
  }(counter));

  // Wait for all to complete
  co_await sleep(200ms);
  LOG("test_concurrent: done, counter = %d", counter.load());
  ASSERT(counter == 3);
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
  ASSERT(completed.load(std::memory_order_relaxed) == 16);
  ASSERT(debug_coro_promise::debug_coro_leak.empty());
}

/// ============================================================================
/// Main test runner
/// ============================================================================

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

int main() {
  test_strand_interoperability(1);
  test_strand_interoperability(4);
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

  // Auto-stop when all tests complete or timeout after 5 seconds
  std::thread([&io_ctx, &tests_completed] {
    auto start = std::chrono::steady_clock::now();
    while (tests_completed < TOTAL_TESTS) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      auto elapsed = std::chrono::steady_clock::now() - start;
      if (elapsed > std::chrono::seconds(5)) {
        LOG("ERROR: Test timeout! Only %d/%d tests completed", tests_completed.load(), TOTAL_TESTS);
        break;
      }
    }
    io_ctx.stop();
  }).detach();

  LOG("Running io_context...");
  auto work = asio::make_work_guard(io_ctx);
  io_ctx.run();

  // Verify all tests completed
  if (tests_completed == TOTAL_TESTS) {
    LOG("=== All %d tests passed! ===", TOTAL_TESTS);
  } else {
    LOG("=== ERROR: Only %d/%d tests completed ===", tests_completed.load(), TOTAL_TESTS);
    return 1;
  }

  // Check for coroutine leaks
  debug_coro_promise::dump();

  return 0;
}
