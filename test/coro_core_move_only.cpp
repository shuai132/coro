#define CORO_DEBUG_PROMISE_LEAK

#include <memory>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "coro/executor_loop.hpp"
#include "coro/executor_poll.hpp"
#include "coro/time.hpp"
#include "coro/when.hpp"
#include "log.h"
#include "utils.hpp"

using namespace coro;

template <typename T>
async<T> completed_task(int& runs, bool delayed) {
  ++runs;
  if (delayed) {
    co_await sleep(0ns);
  }
  if constexpr (std::is_void_v<T>) {
    co_return;
  } else if constexpr (std::is_same_v<T, std::unique_ptr<int>>) {
    co_return std::make_unique<int>(42);
  } else {
    co_return 42;
  }
}

async<void> await_completed_tasks(bool delayed) {
  int void_runs = 0;
  auto empty = completed_task<void>(void_runs, delayed);
  ASSERT(!empty.await_ready());
  co_await empty;
  ASSERT(empty.await_ready());
  co_await empty;
  ASSERT(void_runs == 1);

  int value_runs = 0;
  auto number = completed_task<int>(value_runs, delayed);
  ASSERT((co_await number) == 42);
  ASSERT(number.await_ready());
  ASSERT((co_await number) == 42);
  // A completed frame can also be moved into a combiner without starting again.
  ASSERT((co_await when_all(std::move(number))) == 42);
  ASSERT(value_runs == 1);

  int unique_runs = 0;
  auto unique = completed_task<std::unique_ptr<int>>(unique_runs, delayed);
  auto first = co_await unique;
  ASSERT(first && *first == 42);
  ASSERT(unique.await_ready());
  // Each await moves the stored result; a second await observes the moved-from value.
  ASSERT(!(co_await unique));
  ASSERT(unique_runs == 1);
}

#ifndef CORO_DISABLE_EXCEPTION
template <typename T>
async<T> completed_failure(int& runs, bool delayed) {
  ++runs;
  if (delayed) {
    co_await sleep(0ns);
  }
  // An explicit co_return also identifies the result path to MSVC's coroutine diagnostics.
  co_return []() -> T {
    throw std::runtime_error("completed task failed");
  }();
}

template <typename T>
async<void> await_completed_failure(bool delayed) {
  int runs = 0;
  auto task = completed_failure<T>(runs, delayed);
  for (int attempt = 0; attempt < 3; ++attempt) {
    bool caught = false;
    try {
      co_await task;
    } catch (const std::runtime_error& error) {
      ASSERT(std::string_view(error.what()) == "completed task failed");
      caught = true;
    }
    ASSERT(caught);
    ASSERT(task.await_ready());
  }
  ASSERT(runs == 1);
}

struct retry_result_state {
  bool fail_move = true;
  int live = 0;
};

struct retry_result {
  retry_result_state* state;
  int generation = 0;

  explicit retry_result(retry_result_state& value_state) : state(&value_state) {
    ++state->live;
  }
  retry_result(const retry_result&) = delete;
  retry_result(retry_result&& other) : state(other.state), generation(other.generation + 1) {
    if (state->fail_move && generation == 2) {
      throw std::runtime_error("result extraction failed");
    }
    ++state->live;
  }
  ~retry_result() {
    --state->live;
  }
};

async<retry_result> completed_retry_result(retry_result_state& state, bool delayed) {
  if (delayed) {
    co_await sleep(0ns);
  }
  co_return retry_result{state};
}

async<void> retry_completed_result(bool delayed) {
  retry_result_state state;
  {
    auto task = completed_retry_result(state, delayed);
    bool caught = false;
    try {
      co_await task;
    } catch (const std::runtime_error& error) {
      ASSERT(std::string_view(error.what()) == "result extraction failed");
      caught = true;
    }
    ASSERT(caught);
    ASSERT(task.await_ready());
    state.fail_move = false;
    auto value = co_await task;
    ASSERT(value.state == &state);
  }
  ASSERT(state.live == 0);
}
#endif

enum class completed_launch { detach, spawn, spawn_local, queued_detach, stopped_detach };

template <typename T>
async<T> owned_completed_task(int& runs, bool delayed, [[maybe_unused]] bool fail, [[maybe_unused]] std::shared_ptr<int> frame_token) {
#ifndef CORO_DISABLE_EXCEPTION
  if (fail) {
    co_return co_await completed_failure<T>(runs, delayed);
  }
#endif
  co_return co_await completed_task<T>(runs, delayed);
}

template <typename T>
async<void> launch_completed_task(bool delayed, bool fail, completed_launch mode) {
  auto frame_token = std::make_shared<int>(0);
  int runs = 0;
  auto task = owned_completed_task<T>(runs, delayed, fail, frame_token);
#ifndef CORO_DISABLE_EXCEPTION
  bool caught = false;
  try {
#endif
    co_await task;
#ifndef CORO_DISABLE_EXCEPTION
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "completed task failed");
    caught = true;
  }
  ASSERT(caught == fail);
#endif
  ASSERT(task.await_ready());
  ASSERT(frame_token.use_count() == 2);
  auto* executor = co_await current_executor();
  ASSERT(executor);
  switch (mode) {
    case completed_launch::detach:
      task.detach(*executor);
      break;
    case completed_launch::spawn:
      spawn(*executor, std::move(task));
      break;
    case completed_launch::spawn_local:
      co_await spawn_local(std::move(task));
      break;
    case completed_launch::queued_detach:
    case completed_launch::stopped_detach: {
      executor_poll target;
      if (mode == completed_launch::stopped_detach) {
        target.stop();
      }
      task.bind_executor(target).detach(target);
      if (mode == completed_launch::queued_detach) {
        ASSERT(frame_token.use_count() == 2);
        target.poll();
      }
      ASSERT(frame_token.use_count() == 1);
      break;
    }
  }
  ASSERT(!task.current_coro_handle_);
  ASSERT(frame_token.use_count() == 1);
  ASSERT(runs == 1);
}

struct move_only_value {
  explicit move_only_value(int v) : value(v) {}

  move_only_value(move_only_value&&) noexcept = default;
  move_only_value& operator=(move_only_value&&) noexcept = default;

  move_only_value(const move_only_value&) = delete;
  move_only_value& operator=(const move_only_value&) = delete;

  int value;
};

async<std::unique_ptr<int>> make_unique_value(int value) {
  co_return std::make_unique<int>(value);
}

async<move_only_value> make_move_only_value(int value) {
  co_return move_only_value{value};
}

struct aggregate_result {
  std::unique_ptr<int> value;
  int tag;
};

async<aggregate_result> make_braced_result() {
  co_return {std::make_unique<int>(42), 7};
}

async<int> make_braced_zero() {
  co_return {};
}

async<void> consume_braced_results() {
  auto result = co_await make_braced_result();
  ASSERT(result.value && *result.value == 42);
  ASSERT(result.tag == 7);
  ASSERT((co_await make_braced_zero()) == 0);
}

async<void> noop_task() {
  co_return;
}

async<void> consume_unique_value(int& result) {
  auto value = co_await make_unique_value(42);
  ASSERT(value);
  result = *value;
}

async<void> consume_when_all_move_only_values(int& result) {
  auto [first, second] = co_await when_all(make_move_only_value(10), make_move_only_value(32));
  result = first.value + second.value;
}

async<void> consume_when_all_mixed_move_only_value(int& result) {
  auto value = co_await when_all(noop_task(), make_move_only_value(55), noop_task());
  result = value.value;
}

async<std::tuple<move_only_value, int>> make_tuple_value(int value) {
  co_return std::tuple<move_only_value, int>{move_only_value{value}, 2};
}

async<std::tuple<>> make_empty_tuple() {
  co_return std::tuple<>{};
}

async<void> consume_when_all_tuple_values() {
  auto single = co_await when_all(make_tuple_value(40));
  static_assert(std::is_same_v<decltype(single), std::tuple<move_only_value, int>>);
  ASSERT(std::get<0>(single).value == 40);
  ASSERT(std::get<1>(single) == 2);

  auto mixed = co_await when_all(noop_task(), make_tuple_value(53), noop_task());
  ASSERT(std::get<0>(mixed).value == 53);
  ASSERT(std::get<1>(mixed) == 2);

  auto empty = co_await when_all(noop_task(), make_empty_tuple());
  static_assert(std::is_same_v<decltype(empty), std::tuple<>>);
  auto multiple = co_await when_all(make_tuple_value(61), make_move_only_value(7));
  ASSERT(std::get<0>(std::get<0>(multiple)).value == 61);
  ASSERT(std::get<1>(multiple).value == 7);
}

async<void> consume_when_any_unique_value(int& result) {
  auto any_result = co_await when_any(make_unique_value(77));
  ASSERT(any_result.index == 0);
  auto value = std::move(any_result.template get<0>());
  ASSERT(value);
  result = *value;
}

async<std::monostate> make_monostate_value() {
  co_return std::monostate{};
}

async<void> consume_monostate_value() {
  auto value = co_await make_monostate_value();
  static_assert(std::is_same_v<decltype(value), std::monostate>);
  auto combined = co_await when_any(make_monostate_value());
  ASSERT(combined.index == 0);
  ASSERT(combined.value.index() == 1);
}

#ifndef CORO_DISABLE_EXCEPTION
struct throwing_return_value {
  int* live;

  explicit throwing_return_value(int& count) : live(&count) {
    ++*live;
  }

  throwing_return_value(const throwing_return_value&) = delete;
  throwing_return_value(throwing_return_value&&) {
    throw std::runtime_error("result construction failed");
  }

  ~throwing_return_value() {
    --*live;
  }
};

async<throwing_return_value> make_throwing_return_value(int& live) {
  co_return throwing_return_value{live};
}

async<void> consume_throwing_return_value() {
  int live = 0;
  bool caught = false;
  try {
    co_await make_throwing_return_value(live);
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "result construction failed");
    caught = true;
  }
  ASSERT(caught);
  ASSERT(live == 0);
}

async<std::exception_ptr> make_exception_ptr_value(bool failure) {
  if (failure) {
    throw std::runtime_error("task failure");
  }
  co_return std::make_exception_ptr(std::runtime_error("exception as value"));
}

async<void> consume_exception_ptr_value() {
  auto value = co_await make_exception_ptr_value(false);
  bool caught_value = false;
  try {
    std::rethrow_exception(value);
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "exception as value");
    caught_value = true;
  }
  ASSERT(caught_value);
  bool caught_failure = false;
  try {
    co_await make_exception_ptr_value(true);
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "task failure");
    caught_failure = true;
  }
  ASSERT(caught_failure);
}
#endif

async<void> run_all_tests(int& unique_result, int& when_all_sum, int& mixed_result, int& when_any_result) {
  for (bool delayed : {false, true}) {
    co_await await_completed_tasks(delayed);
    for (auto mode : {completed_launch::detach, completed_launch::spawn, completed_launch::spawn_local, completed_launch::queued_detach,
                      completed_launch::stopped_detach}) {
      co_await launch_completed_task<void>(delayed, false, mode);
      co_await launch_completed_task<int>(delayed, false, mode);
      co_await launch_completed_task<std::unique_ptr<int>>(delayed, false, mode);
#ifndef CORO_DISABLE_EXCEPTION
      co_await launch_completed_task<void>(delayed, true, mode);
      co_await launch_completed_task<int>(delayed, true, mode);
      co_await launch_completed_task<std::unique_ptr<int>>(delayed, true, mode);
#endif
    }
#ifndef CORO_DISABLE_EXCEPTION
    co_await await_completed_failure<void>(delayed);
    co_await await_completed_failure<int>(delayed);
    co_await retry_completed_result(delayed);
#endif
  }
  co_await consume_braced_results();
  co_await consume_unique_value(unique_result);
  co_await consume_when_all_move_only_values(when_all_sum);
  co_await consume_when_all_mixed_move_only_value(mixed_result);
  co_await consume_when_any_unique_value(when_any_result);
  co_await consume_when_all_tuple_values();
  co_await consume_monostate_value();
#ifndef CORO_DISABLE_EXCEPTION
  co_await consume_exception_ptr_value();
  co_await consume_throwing_return_value();
#endif
}

int main() {
  executor_loop executor;
  int unique_result = 0;
  int when_all_sum = 0;
  int mixed_result = 0;
  int when_any_result = 0;

  run_all_tests(unique_result, when_all_sum, mixed_result, when_any_result).detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();

  ASSERT(unique_result == 42);
  ASSERT(when_all_sum == 42);
  ASSERT(mixed_result == 55);
  ASSERT(when_any_result == 77);
  check_coro_leak();
  return 0;
}
