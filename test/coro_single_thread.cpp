#define CORO_DEBUG_PROMISE_LEAK

#include <memory>
#include <thread>
#include <tuple>
#include <type_traits>

#include "assert_def.h"
#include "coro.hpp"
#include "log.h"
#include "retained_child.hpp"
#include "utils.hpp"

#ifndef CORO_SINGLE_THREAD
#error This regression requires CORO_SINGLE_THREAD
#endif

using namespace coro;
using all_state = detail::when_all_state<int>;
using all_owner = decltype(std::declval<detail::when_all_awaiter<all_state, int>>().state_);
using any_state = detail::when_any_state<int>;
using any_owner = decltype(std::declval<detail::when_any_awaiter<any_state, detail::when_any_result<int>, int>>().state_);
using local_owner = decltype(std::declval<async<void>::promise_type>().local_storage_);
static_assert(std::is_same_v<all_owner, detail::shared_ptr_st<all_state>>);
static_assert(std::is_same_v<any_owner, detail::shared_ptr_st<any_state>>);
static_assert(std::is_same_v<local_owner, detail::shared_ptr_st<detail::coro_local_map>>);
static_assert(std::is_same_v<detail::when_mutex, dummy_mutex>);
static_assert(std::is_same_v<detail::when_counter, detail::atomic_st<size_t>>);
static_assert(std::is_same_v<detail::when_flag, detail::atomic_st<bool>>);

// A callback copy may be destroyed on the posting thread after the worker consumes it.
struct copying_executor : executor_loop {
  void dispatch(std::function<void()> fn) override {
    auto copy = fn;
    executor_loop::dispatch(std::move(copy));
  }
};

struct owned_value {
  int& live_;
  explicit owned_value(int& live) : live_(live) {
    ++live_;
  }
  ~owned_value() {
    --live_;
  }
};

async<void> yield_once() {
  co_await callback_awaiter<void>([](executor* exec, auto callback) {
    exec->post(std::move(callback));
  });
}

async<int> value_task(int value) {
  co_return value;
}

async<void> local_child(coro_local<int>& value) {
  ASSERT((co_await value.get()) == 42);
  co_await value.set(99);
}

async<std::unique_ptr<owned_value>> losing_task(event_st& release, int& live, bool& completed) {
  auto value = std::make_unique<owned_value>(live);
  co_await release;
  completed = true;
  co_return std::move(value);
}

async<void> race_parent(event_st& release, int& live, bool& completed) {
  auto result = co_await when_any(value_task(7), losing_task(release, live, completed));
  ASSERT(result.index == 0);
  ASSERT(result.get<0>() == 7);
}

async<void> test_runtime(std::thread::id worker) {
  ASSERT(std::this_thread::get_id() == worker);
  coro_local<int> value;
  co_await value.set(42);
  co_await local_child(value);
  ASSERT((co_await value.get()) == 42);

  auto values = co_await when_all(value_task(1), value_task(2), yield_once());
  ASSERT(std::get<0>(values) == 1 && std::get<1>(values) == 2);
  co_await when_all(yield_once(), yield_once());
  auto first_void = co_await when_any(yield_once(), yield_once());
  ASSERT(first_void.index < 2);

  event_st release;
  int live = 0;
  bool completed = false;
  co_await race_parent(release, live, completed);
  ASSERT(live == 1 && !completed);
  release.set();
  co_await yield_once();
  ASSERT(completed && live == 0);
}

int main() {
  for (bool worker : {false, true}) {
    retained_child_test::run<int>(false, worker);
    retained_child_test::run<void>(false, worker);
#ifndef CORO_DISABLE_EXCEPTION
    retained_child_test::run<int>(true, worker);
    retained_child_test::run<void>(true, worker);
#endif
    check_coro_leak();
  }
  copying_executor exec;
  std::thread worker([&] {
    exec.run_loop();
  });
  // All coroutine state stays on worker, while ownership of the launch callback crosses threads.
  test_runtime(worker.get_id()).detach_with_callback(exec, [&] {
    exec.stop();
  });
  worker.join();
  check_coro_leak();
  return 0;
}
