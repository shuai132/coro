/// config debug
#define CORO_DEBUG_PROMISE_LEAK
#include "TimeCount.hpp"
#include "assert_def.h"
#include "coro.hpp"
#include "coro/when.hpp"
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
  ASSERT(elapsed >= 30 && elapsed < 80);

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
  ASSERT(elapsed >= 20 && elapsed < 50);

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
  auto result = co_await when_any(delayed_value_task(100, 50), delayed_void_task("FastVoid", 15), delayed_value_task(200, 40));

  auto elapsed = t.elapsed();
  LOG("First task completed after %d ms", (int)elapsed);
  LOG("Completed task index: %zu", result.index);

  // The void task (index 1) should complete first
  ASSERT(result.index == 1);
  ASSERT(elapsed >= 15 && elapsed < 40);

  LOG("when_any mixed types test: PASSED");
}

async<void> run_all_tests() {
  co_await test_when_all_values();
  co_await test_when_all_void();
  co_await test_when_any_values();
  co_await test_when_any_void();
  co_await test_when_all_mixed_types();
  co_await test_when_all_mixed_types2();
  co_await test_when_any_mixed_types();

  LOG("=== ALL TESTS PASSED ===");
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
