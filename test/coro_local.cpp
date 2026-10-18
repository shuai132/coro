/// config debug
#define CORO_DEBUG_PROMISE_LEAK
#include "coro/coro_local.hpp"

#include <any>
#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "coro.hpp"
#include "detail/assert_def.h"
#include "detail/log.h"
#include "detail/noinline.hpp"
#include "detail/utils.hpp"

using namespace coro;

thread_local bool count_local_allocations = false;
thread_local size_t local_allocations = 0;
#ifndef CORO_DISABLE_EXCEPTION
thread_local bool fail_local_allocation = false;
#endif

CORO_TEST_NOINLINE void* operator new(std::size_t size) {
#ifndef CORO_DISABLE_EXCEPTION
  if (std::exchange(fail_local_allocation, false)) {
    throw std::bad_alloc();
  }
#endif
  if (count_local_allocations) {
    ++local_allocations;
  }
  if (auto* memory = std::malloc(size ? size : 1)) {
    return memory;
  }
#ifndef CORO_DISABLE_EXCEPTION
  throw std::bad_alloc();
#else
  std::abort();
#endif
}

CORO_TEST_NOINLINE void operator delete(void* memory) noexcept {
  std::free(memory);
}

CORO_TEST_NOINLINE void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}

async<void> read_inherited_local(coro_local<int>& key) {
  ASSERT(co_await key.has());
  ASSERT((co_await key.get_optional()) == 42);
  co_await key.erase();
  ASSERT((co_await key.get()) == 42);
  auto* borrowed = co_await key.get_ptr();
  ASSERT(borrowed && *borrowed == 42);
  *borrowed = 43;
}

async<void> forward_inherited_local(async<void>& child) {
  co_await child;
}

async<void> await_read_only_local_chain(coro_local<int>& key, async<void>& child) {
  co_await key.set(42);
  local_allocations = 0;
  count_local_allocations = true;
  co_await child;
  count_local_allocations = false;
  ASSERT(local_allocations == 0);
  ASSERT((co_await key.get()) == 43);
}

void test_read_only_local_inheritance() {
  coro_local<int> key;
  {
    auto leaf = read_inherited_local(key);
    auto middle = forward_inherited_local(leaf);
    auto parent = await_read_only_local_chain(key, middle);
    parent.current_coro_handle_.resume();
    ASSERT(parent.current_coro_handle_.done());
    parent.await_resume();
  }
  check_coro_leak();
}

async<void> override_inherited_local(coro_local<int>& key, bool explicit_layer) {
  co_await key.erase();
  ASSERT((co_await key.get()) == 42);
  if (explicit_layer) {
    co_await inherit_coro_local();
    co_await key.erase();
    ASSERT((co_await key.get()) == 42);
  }
  co_await key.set(17);
  ASSERT((co_await key.get()) == 17);
  co_await key.erase();
  ASSERT((co_await key.get()) == 42);
  co_await key.set(23);
}

#ifndef CORO_DISABLE_EXCEPTION
async<void> retry_lazy_local_override(coro_local<int>& key) {
  fail_local_allocation = true;
  bool caught = false;
  try {
    co_await key.set(17);
  } catch (const std::bad_alloc&) {
    caught = true;
  }
  ASSERT(caught && !fail_local_allocation);
  ASSERT((co_await key.get()) == 42);
  co_await key.erase();
  ASSERT((co_await key.get()) == 42);
  co_await key.set(23);
  ASSERT((co_await key.get()) == 23);
}
#endif

async<void> test_lazy_local_overrides() {
  coro_local<int> key;
  co_await key.set(42);
  for (bool explicit_layer : {false, true}) {
    co_await override_inherited_local(key, explicit_layer);
    ASSERT((co_await key.get()) == 42);
  }
#ifndef CORO_DISABLE_EXCEPTION
  co_await retry_lazy_local_override(key);
  ASSERT((co_await key.get()) == 42);
#endif
}

async<void> borrow_local_resource(coro_local<std::shared_ptr<int>>& key) {
  auto* resource = co_await key.get_ptr();
  ASSERT(resource && **resource == 42);
}

using read_only_local_children = std::array<std::optional<async<void>>, 2>;

async<void> retain_read_only_children(coro_local<std::shared_ptr<int>>& key, read_only_local_children& children, std::weak_ptr<int>& resource) {
  co_await key.set(std::make_shared<int>(42));
  resource = *(co_await key.get_ptr());
  for (auto& child : children) {
    child.emplace(borrow_local_resource(key));
    co_await *child;
  }
}

void test_read_only_local_lifetime([[maybe_unused]] bool concurrent_release) {
  coro_local<std::shared_ptr<int>> key;
  read_only_local_children children;
  std::weak_ptr<int> resource;
  {
    auto parent = retain_read_only_children(key, children, resource);
    parent.current_coro_handle_.resume();
    ASSERT(parent.current_coro_handle_.done());
    parent.await_resume();
  }
  ASSERT(!resource.expired());
#ifndef CORO_SINGLE_THREAD
  if (concurrent_release) {
    std::atomic<bool> release{false};
    std::array<std::thread, 2> workers;
    for (size_t index = 0; index < workers.size(); ++index) {
      workers[index] = std::thread([&, index] {
        while (!release.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        children[index].reset();
      });
    }
    release.store(true, std::memory_order_release);
    for (auto& worker : workers) {
      worker.join();
    }
  } else
#endif
  {
    children.front().reset();
    ASSERT(!resource.expired());
    children.back().reset();
  }
  ASSERT(resource.expired());
  check_coro_leak();
}

// Define global coro_local keys for testing
static coro_local<int> int_storage;
static coro_local<std::string> string_storage;
static coro_local<double> double_storage;

struct local_cleanup_state {
  std::weak_ptr<int> inherited_resource;
  bool checking = false;
  bool ancestor_expired_during_cleanup = false;
  int checked_destructors = 0;
};

struct local_cleanup_observer {
  local_cleanup_state* state;
  std::weak_ptr<int> inherited_resource;

  ~local_cleanup_observer() {
    if (state->checking) {
      state->ancestor_expired_during_cleanup |= inherited_resource.expired();
      ++state->checked_destructors;
    }
  }
};

async<void> local_cleanup_child(coro_local<std::shared_ptr<int>>& resource, coro_local<local_cleanup_observer>& observer,
                                local_cleanup_state& state) {
  auto* inherited = co_await resource.get_ptr();
  ASSERT(inherited && *inherited && **inherited == 42);
  state.inherited_resource = *inherited;
  co_await observer.set(local_cleanup_observer{&state, *inherited});
}

async<void> local_cleanup_parent(async<void>& child, coro_local<std::shared_ptr<int>>& resource, coro_local<local_cleanup_observer>& observer,
                                 local_cleanup_state& state, bool initialize) {
  if (initialize) {
    auto inherited = std::make_shared<int>(42);
    state.inherited_resource = inherited;
    co_await resource.set(std::move(inherited));
  } else {
    co_await observer.set(local_cleanup_observer{&state, state.inherited_resource});
  }
  co_await child;
}

void test_inherited_local_cleanup(int depth, bool release_parents_first, int retained_parent = 0) {
  coro_local<std::shared_ptr<int>> resource;
  coro_local<local_cleanup_observer> observer;
  local_cleanup_state state;
  std::vector<std::optional<async<void>>> tasks;
  tasks.reserve(static_cast<size_t>(depth) + 1);
  tasks.emplace_back(local_cleanup_child(resource, observer, state));
  for (int i = 1; i <= depth; ++i) {
    tasks.emplace_back(local_cleanup_parent(*tasks.back(), resource, observer, state, i == depth));
  }
  tasks.back()->current_coro_handle_.resume();
  for (const auto& task : tasks) {
    ASSERT(task->current_coro_handle_.done());
  }
  auto release_parents = [&] {
    for (int i = depth; i > 0; --i) {
      if (i != retained_parent) {
        tasks[static_cast<size_t>(i)].reset();
      }
    }
  };
  if (release_parents_first) {
    release_parents();
  }
  ASSERT(!state.inherited_resource.expired());
  state.checking = true;
  tasks.front().reset();
  ASSERT(state.checked_destructors == (release_parents_first ? (retained_parent ? retained_parent : depth) : 1));
  ASSERT(!state.ancestor_expired_during_cleanup);
  release_parents();
  if (retained_parent) {
    ASSERT(!state.inherited_resource.expired());
    tasks[static_cast<size_t>(retained_parent)].reset();
  }
  ASSERT(state.checked_destructors == depth);
  ASSERT(!state.ancestor_expired_during_cleanup);
  ASSERT(state.inherited_resource.expired());
  check_coro_leak();
}

using local_cleanup_children = std::array<std::optional<async<void>>, 2>;

async<void> local_cleanup_shared_parent(local_cleanup_children& children, coro_local<std::shared_ptr<int>>& resource,
                                        coro_local<local_cleanup_observer>& observer, std::array<local_cleanup_state, 2>& states) {
  co_await resource.set(std::make_shared<int>(42));
  for (size_t index = 0; index < children.size(); ++index) {
    children[index].emplace(local_cleanup_child(resource, observer, states[index]));
    co_await *children[index];
  }
}

void test_shared_inherited_local_cleanup() {
  coro_local<std::shared_ptr<int>> resource;
  coro_local<local_cleanup_observer> observer;
  std::array<local_cleanup_state, 2> states;
  local_cleanup_children children;
  {
    auto parent = local_cleanup_shared_parent(children, resource, observer, states);
    parent.current_coro_handle_.resume();
    ASSERT(parent.current_coro_handle_.done());
    parent.await_resume();
  }
  for (const auto& child : children) {
    ASSERT(child->current_coro_handle_.done());
  }
  for (auto& state : states) {
    ASSERT(!state.inherited_resource.expired());
    state.checking = true;
  }
#ifdef CORO_SINGLE_THREAD
  for (auto& child : children) {
    child.reset();
  }
#else
  std::atomic<bool> release{false};
  std::array<std::thread, 2> workers;
  for (size_t index = 0; index < workers.size(); ++index) {
    workers[index] = std::thread([&, index] {
      while (!release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      children[index].reset();
    });
  }
  release.store(true, std::memory_order_release);
  for (auto& worker : workers) {
    worker.join();
  }
#endif
  for (const auto& state : states) {
    ASSERT(state.checked_destructors == 1);
    ASSERT(!state.ancestor_expired_during_cleanup);
    ASSERT(state.inherited_resource.expired());
  }
  check_coro_leak();
}

async<void> check_distinct_local_keys(coro_local<int>& number, coro_local<std::string>& text) {
  co_await number.set(42);
  co_await text.set("independent");
  ASSERT((co_await number.get()) == 42);
  ASSERT((co_await text.get()) == "independent");
  co_await number.erase();
  ASSERT(!(co_await number.has()));
  ASSERT(co_await text.has());
  ASSERT((co_await text.get()) == "independent");
}

async<void> coro_local_key_identity_test() {
  static std::tuple<coro_local<int>, coro_local<std::string>> tuple_keys;
  co_await check_distinct_local_keys(std::get<0>(tuple_keys), std::get<1>(tuple_keys));

  struct packed_keys {
    [[no_unique_address]] coro_local<int> number;
    [[no_unique_address]] coro_local<std::string> text;
  };
  static packed_keys packed;
  co_await check_distinct_local_keys(packed.number, packed.text);
}

async<void> coro_local_any_child(coro_local<std::any>& storage) {
  auto inherited = co_await storage.get();
  auto* value = std::any_cast<int>(&inherited);
  ASSERT(value && *value == 42);
  co_await storage.set(std::any{std::string("child")});
  auto child = co_await storage.get_optional();
  ASSERT(child);
  auto* text = std::any_cast<std::string>(&*child);
  ASSERT(text && *text == "child");
}

async<void> coro_local_erased_type_test() {
  static coro_local<std::any> storage;
  ASSERT(!(co_await storage.has()));
  co_await storage.set(std::any{42});
  ASSERT(co_await storage.has());
  auto* stored = co_await storage.get_ptr();
  ASSERT(stored);
  auto* value = std::any_cast<int>(stored);
  ASSERT(value && *value == 42);
  co_await coro_local_any_child(storage);
  auto parent = co_await storage.get();
  value = std::any_cast<int>(&parent);
  ASSERT(value && *value == 42);

  co_await storage.set(std::any{});
  ASSERT(co_await storage.has());
  auto empty = co_await storage.get_optional();
  ASSERT(empty && !empty->has_value());
  co_await storage.erase();
  ASSERT(!(co_await storage.get_optional()));

  auto resource = std::make_shared<int>(7);
  co_await storage.set(std::any{resource});
  ASSERT(resource.use_count() == 2);
  co_await storage.erase();
  ASSERT(resource.use_count() == 1);

  // Constructor tags are values here, not instructions to construct another any payload.
  static coro_local<std::in_place_type_t<int>> tag_storage;
  co_await tag_storage.set(std::in_place_type<int>);
  ASSERT((co_await tag_storage.get_ptr()) != nullptr);
  ASSERT(co_await tag_storage.get_optional());
}

void test_deep_local_storage_lookup_and_cleanup() {
  // Build the deep chain directly: GCC sanitizer instrumentation can prevent tail
  // calls during coroutine transfers, overflowing the stack before cleanup starts.
  using storage_map = detail::coro_local_map;
  std::vector<detail::shared_ptr<storage_map>> maps;
  constexpr int depth = 200000;
  local_cleanup_state state;
  maps.reserve(depth);
  maps.push_back(detail::make_shared<storage_map>());
  int key = 0;
  int missing_key = 0;
  int resource_key = 0;
  int observer_key = 0;
  maps.front()->set<int>(&key, 42);
  auto resource = std::make_shared<int>(42);
  state.inherited_resource = resource;
  maps.front()->set<std::shared_ptr<int>>(&resource_key, std::move(resource));
  for (int index = 1; index < depth; ++index) {
    auto map = detail::make_shared<storage_map>();
    map->parent = maps.back();
    map->set<local_cleanup_observer>(&observer_key, local_cleanup_observer{&state, state.inherited_resource});
    maps.push_back(std::move(map));
  }
  auto* value = maps.back()->find<int>(&key);
  ASSERT(value && *value == 42);
  ASSERT(!maps.back()->find<int>(&missing_key));
  ASSERT(maps.back()->contains(&key));
  ASSERT(!maps.back()->contains(&missing_key));
  // Drop ancestor owners first so the leaf must release the entire inherited chain.
  for (int index = 0; index < depth - 1; ++index) {
    maps[static_cast<size_t>(index)] = {};
  }
  ASSERT(!state.inherited_resource.expired());
  state.checking = true;
  maps.back() = {};
  ASSERT(state.checked_destructors == depth - 1);
  ASSERT(!state.ancestor_expired_during_cleanup);
  ASSERT(state.inherited_resource.expired());
}

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

  auto getting = double_storage.get_optional();
  // get_optional should return nullopt when not set
  auto opt = co_await getting;
  ASSERT(!opt.has_value());

  // Set a value
  co_await double_storage.set(3.14);

  // Now get_optional should return the value
  opt = co_await getting;
  ASSERT(opt.has_value());
  ASSERT(opt.value() == 3.14);

  co_await double_storage.erase();
  opt = co_await getting;
  ASSERT(!opt.has_value());

  co_await double_storage.set(2.71);
  opt = co_await getting;
  ASSERT(opt.has_value());
  ASSERT(opt.value() == 2.71);

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

struct nonassignable_local_value {
  int value;

  explicit nonassignable_local_value(int v = 0) : value(v) {}
  nonassignable_local_value(const nonassignable_local_value&) = default;
  nonassignable_local_value(nonassignable_local_value&&) = default;
  nonassignable_local_value& operator=(const nonassignable_local_value&) = delete;
  nonassignable_local_value& operator=(nonassignable_local_value&&) = delete;
};

struct default_counting_local_value {
  inline static int defaults = 0;
  int value;

  default_counting_local_value() : value(0) {
    ++defaults;
  }

  explicit default_counting_local_value(int v) : value(v) {}
};

async<void> read_inherited_without_default(coro_local<default_counting_local_value>& storage) {
  auto value = co_await storage.get();
  ASSERT(value.value == 42);
  ASSERT(default_counting_local_value::defaults == 0);
}

async<void> coro_local_lazy_default_test() {
  static coro_local<default_counting_local_value> storage;
  default_counting_local_value::defaults = 0;
  co_await storage.set(42);
  auto value = co_await storage.get();
  ASSERT(value.value == 42);
  ASSERT(default_counting_local_value::defaults == 0);
  co_await read_inherited_without_default(storage);
  co_await storage.erase();
  auto missing = co_await storage.get();
  ASSERT(missing.value == 0);
  ASSERT(default_counting_local_value::defaults == 1);
}

async<void> coro_local_nonassignable_test() {
  static coro_local<nonassignable_local_value> storage;
  auto missing = co_await storage.get();
  ASSERT(missing.value == 0);
  auto missing_optional = co_await storage.get_optional();
  ASSERT(!missing_optional);
  co_await storage.set(42);
  auto value = co_await storage.get();
  ASSERT(value.value == 42);
  auto optional = co_await storage.get_optional();
  ASSERT(optional && optional->value == 42);
  nonassignable_local_value replacement{53};
  co_await storage.set(replacement);
  auto updated = co_await storage.get();
  ASSERT(updated.value == 53);
  co_await storage.erase();
  ASSERT(!(co_await storage.has()));
}

#ifndef CORO_DISABLE_EXCEPTION
struct throwing_local_value {
  inline static int live = 0;
  inline static bool fail_copy = false;
  inline static bool fail_move = false;
  inline static bool fail_default = false;
  int value = 0;

  throwing_local_value() {
    if (fail_default) {
      throw std::runtime_error("local default failed");
    }
    ++live;
  }

  explicit throwing_local_value(int v) : value(v) {
    if (v < 0) {
      throw std::runtime_error("local conversion failed");
    }
    ++live;
  }

  throwing_local_value(const throwing_local_value& other) : value(other.value) {
    if (fail_copy) {
      throw std::runtime_error("local copy failed");
    }
    ++live;
  }

  throwing_local_value(throwing_local_value&& other) : value(other.value) {
    if (fail_move) {
      throw std::runtime_error("local move failed");
    }
    ++live;
  }

  throwing_local_value& operator=(const throwing_local_value& other) {
    if (fail_copy) {
      throw std::runtime_error("local copy failed");
    }
    value = other.value;
    return *this;
  }

  ~throwing_local_value() {
    --live;
  }
};

async<void> coro_local_failed_shadow(coro_local<throwing_local_value>& storage) {
  throwing_local_value::fail_move = true;
  bool caught = false;
  try {
    co_await storage.set(53);
  } catch (const std::runtime_error&) {
    caught = true;
  }
  throwing_local_value::fail_move = false;
  ASSERT(caught);
  auto* inherited = co_await storage.get_ptr();
  ASSERT(inherited && inherited->value == 42);
}

async<void> coro_local_failed_set_test() {
  static coro_local<throwing_local_value> storage;
  throwing_local_value::fail_move = true;
  bool caught = false;
  try {
    co_await storage.set(7);
  } catch (const std::runtime_error&) {
    caught = true;
  }
  throwing_local_value::fail_move = false;
  ASSERT(caught);
  ASSERT(!(co_await storage.has()));
  ASSERT((co_await storage.get_ptr()) == nullptr);
  ASSERT(throwing_local_value::live == 0);

  co_await storage.set(42);
  co_await coro_local_failed_shadow(storage);
  throwing_local_value::fail_move = true;
  caught = false;
  try {
    co_await storage.set(99);
  } catch (const std::runtime_error&) {
    caught = true;
  }
  throwing_local_value::fail_move = false;
  ASSERT(caught);
  auto* previous = co_await storage.get_ptr();
  ASSERT(previous && previous->value == 42);
  co_await storage.erase();
  ASSERT(throwing_local_value::live == 0);
}

async<void> coro_local_exception_test() {
  static coro_local<throwing_local_value> storage;
  ASSERT(throwing_local_value::live == 0);
  co_await storage.set(7);
  bool caught = false;
  try {
    co_await storage.set(-1);
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "local conversion failed");
    caught = true;
  }
  ASSERT(caught);

  throwing_local_value::fail_default = true;
  caught = false;
  try {
    auto stored = co_await storage.get();
    ASSERT(stored.value == 7);
  } catch (...) {
    caught = true;
  }
  ASSERT(!caught);
  throwing_local_value::fail_default = false;

  for (bool fail_copy : {true, false}) {
    throwing_local_value::fail_copy = fail_copy;
    throwing_local_value::fail_move = !fail_copy;
    caught = false;
    try {
      [[maybe_unused]] auto value = co_await storage.get();
    } catch (const std::runtime_error& error) {
      ASSERT(std::string_view(error.what()) == (fail_copy ? "local copy failed" : "local move failed"));
      caught = true;
    }
    ASSERT(caught);
    caught = false;
    try {
      [[maybe_unused]] auto value = co_await storage.get_optional();
    } catch (const std::runtime_error& error) {
      ASSERT(std::string_view(error.what()) == (fail_copy ? "local copy failed" : "local move failed"));
      caught = true;
    }
    ASSERT(caught);
  }
  throwing_local_value::fail_copy = false;
  throwing_local_value::fail_move = false;
  {
    throwing_local_value value{5};
    throwing_local_value::fail_move = true;
    caught = false;
    try {
      co_await storage.set(std::move(value));
    } catch (const std::runtime_error& error) {
      ASSERT(std::string_view(error.what()) == "local move failed");
      caught = true;
    }
    ASSERT(caught);
    throwing_local_value::fail_move = false;
  }
  auto* value = co_await storage.get_ptr();
  ASSERT(value && value->value == 7);
  co_await storage.erase();
  throwing_local_value::fail_default = true;
  caught = false;
  try {
    [[maybe_unused]] auto missing = co_await storage.get();
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "local default failed");
    caught = true;
  }
  throwing_local_value::fail_default = false;
  ASSERT(caught);
  ASSERT(throwing_local_value::live == 0);
}
#endif

async<void> run_all_tests() {
  co_await test_lazy_local_overrides();
  co_await coro_local_key_identity_test();
  co_await coro_local_erased_type_test();
  co_await coro_local_lazy_default_test();
  co_await coro_local_basic_test();
  co_await coro_local_nonassignable_test();
#ifndef CORO_DISABLE_EXCEPTION
  co_await coro_local_exception_test();
  co_await coro_local_failed_set_test();
#endif
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
  test_read_only_local_inheritance();
  test_read_only_local_lifetime(false);
#ifndef CORO_SINGLE_THREAD
  test_read_only_local_lifetime(true);
#endif
  test_shared_inherited_local_cleanup();
  for (int depth : {1, 4}) {
    for (bool release_parents_first : {false, true}) {
      test_inherited_local_cleanup(depth, release_parents_first);
    }
  }
  test_inherited_local_cleanup(4, true, 2);
  test_deep_local_storage_lookup_and_cleanup();
  LOG("Local Storage test init");
  executor_loop executor;
  run_all_tests().detach(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  check_coro_leak();
  return 0;
}
