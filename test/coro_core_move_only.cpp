#define CORO_DEBUG_PROMISE_LEAK

#include <memory>
#include <tuple>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "coro/executor_loop.hpp"
#include "coro/when.hpp"
#include "log.h"
#include "utils.hpp"

using namespace coro;

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

async<void> run_all_tests(int& unique_result, int& when_all_sum, int& mixed_result, int& when_any_result) {
  co_await consume_unique_value(unique_result);
  co_await consume_when_all_move_only_values(when_all_sum);
  co_await consume_when_all_mixed_move_only_value(mixed_result);
  co_await consume_when_any_unique_value(when_any_result);
  co_await consume_when_all_tuple_values();
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
