/// config debug
#define CORO_DEBUG_PROMISE_LEAK
#include "coro/coro_local.hpp"

#include <string>

#include "coro.hpp"
#include "detail/assert_def.h"
#include "detail/log.h"
#include "detail/utils.hpp"

using namespace coro;

// Define global coro_local keys for testing
static coro_local<int> int_storage;
static coro_local<std::string> string_storage;
static coro_local<double> double_storage;

// Test 1: Basic set and get
async<void> coro_local_basic_test() {
  LOG("=== Basic Local Storage Test ===");

  // Initially, value should be default-constructed
  int val = co_await int_storage.get();
  ASSERT(val == 0);

  // Check has() returns false initially
  bool has = co_await int_storage.has();
  ASSERT(!has);

  // Set a value
  co_await int_storage.set(42);

  // Get the value back
  val = co_await int_storage.get();
  ASSERT(val == 42);

  // has() should return true now
  has = co_await int_storage.has();
  ASSERT(has);

  LOG("Basic test: OK");
  co_return;
}

// Test 2: get_optional returns nullopt when not set
async<void> coro_local_optional_test() {
  LOG("=== Optional Local Storage Test ===");

  // get_optional should return nullopt when not set
  auto opt = co_await double_storage.get_optional();
  ASSERT(!opt.has_value());

  // Set a value
  co_await double_storage.set(3.14);

  // Now get_optional should return the value
  opt = co_await double_storage.get_optional();
  ASSERT(opt.has_value());
  ASSERT(opt.value() == 3.14);

  LOG("Optional test: OK");
  co_return;
}

// Test 3: get_ptr returns nullptr when not set
async<void> coro_local_ptr_test() {
  LOG("=== Pointer Local Storage Test ===");

  static coro_local<int> ptr_test_storage;

  // get_ptr should return nullptr when not set
  int* ptr = co_await ptr_test_storage.get_ptr();
  ASSERT(ptr == nullptr);

  // Set a value
  co_await ptr_test_storage.set(100);

  // Now get_ptr should return a valid pointer
  ptr = co_await ptr_test_storage.get_ptr();
  ASSERT(ptr != nullptr);
  ASSERT(*ptr == 100);

  LOG("Pointer test: OK");
  co_return;
}

// Test 4: String storage
async<void> coro_local_string_test() {
  LOG("=== String Local Storage Test ===");

  co_await string_storage.set("Hello, Coroutine!");

  std::string val = co_await string_storage.get();
  ASSERT(val == "Hello, Coroutine!");

  // Overwrite with new value
  co_await string_storage.set("Updated!");
  val = co_await string_storage.get();
  ASSERT(val == "Updated!");

  LOG("String test: OK");
  co_return;
}

// Test 5: Erase functionality
async<void> coro_local_erase_test() {
  LOG("=== Erase Local Storage Test ===");

  static coro_local<int> erase_test_storage;

  co_await erase_test_storage.set(999);

  bool has = co_await erase_test_storage.has();
  ASSERT(has);

  // Erase the value
  co_await erase_test_storage.erase();

  has = co_await erase_test_storage.has();
  ASSERT(!has);

  // get() should return default value after erase
  int val = co_await erase_test_storage.get();
  ASSERT(val == 0);

  LOG("Erase test: OK");
  co_return;
}

// Test 6: Child coroutine inherits parent storage (read-only)
async<void> coro_local_inheritance_test() {
  LOG("=== Inheritance Local Storage Test ===");

  static coro_local<int> inherit_storage;

  co_await inherit_storage.set(123);

  // Child coroutine should see parent's value
  auto child = [&]() -> async<void> {
    int val = co_await inherit_storage.get();
    LOG("Child sees value: %d", val);
    ASSERT(val == 123);
    co_return;
  };

  co_await child();

  LOG("Inheritance test: OK");
  co_return;
}

// Test 7: Child coroutine can shadow parent value (copy-on-write)
async<void> coro_local_shadow_test() {
  LOG("=== Shadow Local Storage Test ===");

  static coro_local<int> shadow_storage;

  co_await shadow_storage.set(100);

  // Child coroutine modifies storage - should not affect parent
  auto child = [&]() -> async<void> {
    // First, inherit parent storage
    co_await inherit_coro_local();

    // Read parent value
    int val = co_await shadow_storage.get();
    ASSERT(val == 100);

    // Set own value (shadow parent)
    co_await shadow_storage.set(200);

    val = co_await shadow_storage.get();
    ASSERT(val == 200);

    co_return;
  };

  co_await child();

  // Parent value should be unchanged
  int parent_val = co_await shadow_storage.get();
  ASSERT(parent_val == 100);

  LOG("Shadow test: OK");
  co_return;
}

// Test 8: Multiple coro_local instances are independent
async<void> coro_local_independence_test() {
  LOG("=== Independence Local Storage Test ===");

  static coro_local<int> storage_a;
  static coro_local<int> storage_b;

  co_await storage_a.set(1);
  co_await storage_b.set(2);

  int val_a = co_await storage_a.get();
  int val_b = co_await storage_b.get();

  ASSERT(val_a == 1);
  ASSERT(val_b == 2);

  // Modifying one should not affect the other
  co_await storage_a.set(10);

  val_a = co_await storage_a.get();
  val_b = co_await storage_b.get();

  ASSERT(val_a == 10);
  ASSERT(val_b == 2);

  LOG("Independence test: OK");
  co_return;
}

// Test 9: Deep nesting inheritance
async<void> coro_local_deep_nesting_test() {
  LOG("=== Deep Nesting Local Storage Test ===");

  static coro_local<int> depth_storage;

  co_await depth_storage.set(0);

  auto nested_coro = [](int depth) -> async<int> {
    auto nested_coro_impl = [](auto& self, int d) -> async<int> {
      int current = co_await depth_storage.get();
      LOG("Depth %d, sees value: %d", d, current);

      if (d <= 0) {
        co_return current;
      }

      // Inherit and shadow
      co_await inherit_coro_local();
      co_await depth_storage.set(current + 1);

      int result = co_await self(self, d - 1);
      co_return result;
    };
    co_return co_await nested_coro_impl(nested_coro_impl, depth);
  };

  int final_val = co_await nested_coro(5);
  LOG("Final value after 5 levels: %d", final_val);
  ASSERT(final_val == 5);

  // Original value should be unchanged
  int original = co_await depth_storage.get();
  ASSERT(original == 0);

  LOG("Deep nesting test: OK");
  co_return;
}

// Test 10: Concurrent coroutines have isolated storage
async<void> coro_local_isolation_test() {
  LOG("=== Isolation Local Storage Test ===");

  static coro_local<int> isolation_storage;

  int completed = 0;

  auto coro_a = [&]() -> async<void> {
    co_await isolation_storage.set(111);
    co_await sleep(50ms);
    int val = co_await isolation_storage.get();
    ASSERT(val == 111);
    completed++;
    co_return;
  };

  auto coro_b = [&]() -> async<void> {
    co_await isolation_storage.set(222);
    co_await sleep(50ms);
    int val = co_await isolation_storage.get();
    ASSERT(val == 222);
    completed++;
    co_return;
  };

  co_await spawn_local(coro_a());
  co_await spawn_local(coro_b());

  co_await sleep(100ms);
  ASSERT(completed == 2);

  LOG("Isolation test: OK");
  co_return;
}

// Test 11: Complex type storage
async<void> coro_local_complex_type_test() {
  LOG("=== Complex Type Local Storage Test ===");

  struct ComplexData {
    int id;
    std::string name;
    std::vector<int> values;

    bool operator==(const ComplexData& other) const {
      return id == other.id && name == other.name && values == other.values;
    }
  };

  static coro_local<ComplexData> complex_storage;

  ComplexData data{42, "TestData", {1, 2, 3, 4, 5}};
  co_await complex_storage.set(data);

  ComplexData retrieved = co_await complex_storage.get();
  ASSERT(retrieved == data);

  LOG("Complex type test: OK");
  co_return;
}

// Test 12: Multiple set operations overwrite correctly
async<void> coro_local_overwrite_test() {
  LOG("=== Overwrite Local Storage Test ===");

  static coro_local<int> overwrite_storage;

  for (int i = 0; i < 10; ++i) {
    co_await overwrite_storage.set(i);
    int val = co_await overwrite_storage.get();
    ASSERT(val == i);
  }

  int final_val = co_await overwrite_storage.get();
  ASSERT(final_val == 9);

  LOG("Overwrite test: OK");
  co_return;
}

async<void> run_all_tests() {
  co_await coro_local_basic_test();
  co_await coro_local_optional_test();
  co_await coro_local_ptr_test();
  co_await coro_local_string_test();
  co_await coro_local_erase_test();
  co_await coro_local_inheritance_test();
  co_await coro_local_shadow_test();
  co_await coro_local_independence_test();
  co_await coro_local_deep_nesting_test();
  co_await coro_local_isolation_test();
  co_await coro_local_complex_type_test();
  co_await coro_local_overwrite_test();

  LOG("\n=== All Local Storage Tests Passed ===");
  co_return;
}

int main() {
  LOG("Local Storage test init");
  executor_loop executor;
  run_all_tests().detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  check_coro_leak();
  return 0;
}
