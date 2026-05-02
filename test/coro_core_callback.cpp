#define CORO_DEBUG_PROMISE_LEAK

#include <functional>
#include <memory>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "coro/executor_loop.hpp"
#include "log.h"
#include "utils.hpp"

using namespace coro;

struct callback_state {
  int call_depth = 0;
  int destructor_during_call = 0;
};

struct synchronous_callback {
  std::shared_ptr<callback_state> state;

  ~synchronous_callback() {
    if (state && state->call_depth > 0) {
      ++state->destructor_during_call;
    }
  }

  void operator()(std::function<void(int)> callback) const {
    ++state->call_depth;
    callback(7);
    --state->call_depth;
  }
};

async<void> await_synchronous_callback(std::shared_ptr<callback_state> state) {
  int value = co_await callback_awaiter<int>(synchronous_callback{state});
  ASSERT(value == 7);
}

struct non_default_move_only_value {
  explicit non_default_move_only_value(int value) : value(value) {}

  non_default_move_only_value(const non_default_move_only_value&) = delete;
  non_default_move_only_value& operator=(const non_default_move_only_value&) = delete;
  non_default_move_only_value(non_default_move_only_value&&) noexcept = default;
  non_default_move_only_value& operator=(non_default_move_only_value&&) noexcept = default;

  int value;
};

async<void> await_synchronous_void_callback(int& completed_count) {
  co_await callback_awaiter<void>([](std::function<void()> callback) {
    callback();
  });
  ++completed_count;
}

async<void> await_executor_aware_callback(executor*& observed_exec) {
  auto* current_exec = co_await current_executor();
  int value = co_await callback_awaiter<int>([&observed_exec](executor* exec, std::function<void(int)> callback) {
    observed_exec = exec;
    exec->post([callback = std::move(callback)]() mutable {
      callback(11);
    });
  });
  ASSERT(observed_exec == current_exec);
  ASSERT(value == 11);
}

async<void> await_asynchronous_value_callback(int& result, bool& callback_started) {
  int value = co_await callback_awaiter<int>([&callback_started](executor* exec, std::function<void(int)> callback) {
    callback_started = true;
    exec->post([callback = std::move(callback)]() mutable {
      callback(19);
    });
  });
  result = value;
}

async<void> await_move_only_callback_value(int& result) {
  auto value = co_await callback_awaiter<std::unique_ptr<int>>([](executor* exec, std::function<void(std::unique_ptr<int>)> callback) {
    exec->post([callback = std::move(callback)]() mutable {
      callback(std::make_unique<int>(23));
    });
  });
  ASSERT(value);
  result = *value;
}

async<void> await_non_default_move_only_callback_value(int& result) {
  auto value = co_await callback_awaiter<non_default_move_only_value>([](std::function<void(non_default_move_only_value)> callback) {
    callback(non_default_move_only_value{29});
  });
  result = value.value;
}

async<void> run_callback_tests(std::shared_ptr<callback_state> state, int& void_completed_count, executor*& observed_exec, int& async_result,
                               bool& async_callback_started, int& move_only_result, int& non_default_result) {
  co_await await_synchronous_callback(state);
  co_await await_synchronous_void_callback(void_completed_count);
  co_await await_executor_aware_callback(observed_exec);
  co_await await_asynchronous_value_callback(async_result, async_callback_started);
  co_await await_move_only_callback_value(move_only_result);
  co_await await_non_default_move_only_callback_value(non_default_result);
}

int main() {
  executor_loop loop;
  auto state = std::make_shared<callback_state>();
  int void_completed_count = 0;
  coro::executor* observed_exec = nullptr;
  int async_result = 0;
  bool async_callback_started = false;
  int move_only_result = 0;
  int non_default_result = 0;

  run_callback_tests(state, void_completed_count, observed_exec, async_result, async_callback_started, move_only_result, non_default_result)
      .detach_with_callback(loop, [&] {
        loop.stop();
      });
  loop.run_loop();

  ASSERT(state->destructor_during_call == 0);
  ASSERT(void_completed_count == 1);
  ASSERT(observed_exec == &loop);
  ASSERT(async_callback_started);
  ASSERT(async_result == 19);
  ASSERT(move_only_result == 23);
  ASSERT(non_default_result == 29);
  check_coro_leak();
  return 0;
}
