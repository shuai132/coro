/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION

#include "TimeCount.hpp"
#include "assert_def.h"
#include "coro.hpp"
#include "log.h"
#include "utils.hpp"

using namespace coro;

// Test task that returns a value after a delay
async<int> delayed_value_task(int value, int delay_ms) {
  LOG("Task %d: starting, will complete after %d ms", value, delay_ms);
  co_await sleep(std::chrono::milliseconds(delay_ms));
  LOG("Task %d: completed", value);
  co_return value;
}

// Test task that returns void
async<void> delayed_void_task(const char* name, int delay_ms) {
  LOG("Task %s: starting, will complete after %d ms", name, delay_ms);
  co_await sleep(std::chrono::milliseconds(delay_ms));
  LOG("Task %s: completed", name);
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

  // Launch 3 tasks with different delays
  auto result = co_await when_any(delayed_value_task(1, 100), delayed_value_task(2, 30), delayed_value_task(3, 80));

  auto elapsed = t.elapsed();
  LOG("First task completed after %d ms", (int)elapsed);
  LOG("Completed task index: %zu", result.index);

  // Task 2 should complete first (30ms delay)
  ASSERT(result.index == 1);  // Index 1 corresponds to the second task
#ifndef CORO_TEST_RUNNER_VERY_SLOW
  ASSERT(elapsed >= 30 && elapsed < 80);
#endif
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

  // Launch 3 void tasks with different delays
  auto result = co_await when_any(delayed_void_task("A", 60), delayed_void_task("B", 20), delayed_void_task("C", 50));

  auto elapsed = t.elapsed();
  LOG("First void task completed after %d ms", (int)elapsed);
  LOG("Completed task index: %zu", result.index);

  // Task B should complete first (20ms delay)
  ASSERT(result.index == 1);  // Index 1 corresponds to task B
#ifndef CORO_TEST_RUNNER_VERY_SLOW
  ASSERT(elapsed >= 20 && elapsed < 50);
#endif

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

  // Launch tasks with different return types
  auto result = co_await when_any(delayed_value_task(100, 50), delayed_void_task("FastVoid", 15), delayed_value_task(200, 60));

  auto elapsed = t.elapsed();
  LOG("First task completed after %d ms", (int)elapsed);
  LOG("Completed task index: %zu", result.index);

  // The void task (index 1) should complete first
  ASSERT(result.index == 1);
#ifndef CORO_TEST_RUNNER_VERY_SLOW
  ASSERT(elapsed >= 15 && elapsed < 50);
#endif

  LOG("when_any mixed types test: PASSED");
}

#ifndef CORO_DISABLE_EXCEPTION

// Exception test tasks
async<int> delayed_value_task_with_exception(int value, int delay_ms, bool should_throw) {
  LOG("Task %d: starting, will complete after %d ms, throw=%d", value, delay_ms, should_throw);
  co_await sleep(std::chrono::milliseconds(delay_ms));
  if (should_throw) {
    LOG("Task %d: throwing exception", value);
    throw std::runtime_error("Task " + std::to_string(value) + " failed");
  }
  LOG("Task %d: completed", value);
  co_return value;
}

async<void> delayed_void_task_with_exception(const char* name, int delay_ms, bool should_throw) {
  LOG("Task %s: starting, will complete after %d ms, throw=%d", name, delay_ms, should_throw);
  co_await sleep(std::chrono::milliseconds(delay_ms));
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

  bool exception_caught = false;
  std::string exception_msg;
  try {
    // Task 2 completes first (20ms) but throws
    [[maybe_unused]] auto result = co_await when_any(delayed_value_task_with_exception(1, 100, false), delayed_value_task_with_exception(2, 20, true),
                                                     delayed_value_task_with_exception(3, 80, false));
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
#ifndef CORO_TEST_RUNNER_VERY_SLOW
  ASSERT(elapsed >= 20 && elapsed < 80);  // Should complete when first task completes
#endif
  LOG("when_any exception first test: PASSED");
}

// Test when_any with exception (first completed task doesn't throw)
async<void> test_when_any_exception_later() {
  LOG("=== Testing when_any with exception (later task throws) ===");
  TimeCount t;

  // Task 2 completes first and succeeds, task 3 throws later (but is ignored)
  auto result = co_await when_any(delayed_value_task_with_exception(1, 100, true), delayed_value_task_with_exception(2, 20, false),
                                  delayed_value_task_with_exception(3, 80, true));

  auto elapsed = t.elapsed();
  LOG("First task completed after %d ms", (int)elapsed);
  LOG("Completed task index: %zu", result.index);

  // Task 2 should complete first (20ms delay) without exception
  ASSERT(result.index == 1);
#ifndef CORO_TEST_RUNNER_VERY_SLOW
  ASSERT(elapsed >= 20 && elapsed < 80);
#endif

  auto value = result.template get<1>();
  LOG("Completed task value: %d", value);
  ASSERT(value == 2);

  LOG("when_any exception later test: PASSED");
}

// Test when_any with void tasks and exception
async<void> test_when_any_void_exception() {
  LOG("=== Testing when_any with void tasks and exception ===");
  TimeCount t;

  bool exception_caught = false;
  std::string exception_msg;
  try {
    // Task B completes first (20ms) and throws
    [[maybe_unused]] auto result =
        co_await when_any(delayed_void_task_with_exception("A", 60, false), delayed_void_task_with_exception("B", 20, true),
                          delayed_void_task_with_exception("C", 50, false));
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
#ifndef CORO_TEST_RUNNER_VERY_SLOW
  ASSERT(elapsed >= 20 && elapsed < 50);  // Should complete when first task completes
#endif
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

async<void> run_all_tests() {
  co_await test_when_all_values();
  co_await test_when_all_void();
  co_await test_when_any_values();
  co_await test_when_any_void();
  co_await test_when_all_mixed_types();
  co_await test_when_all_mixed_types2();
  co_await test_when_any_mixed_types();
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
  LOG("when_all/when_any test init");
  executor_loop executor;
  run_all_tests().detach_with_callback(executor, [&] {
    executor.stop();
  });
  LOG("loop...");
  executor.run_loop();
  check_coro_leak();
  return 0;
}
