#define CORO_DEBUG_PROMISE_LEAK

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "coro/executor_loop.hpp"
#include "coro/executor_poll.hpp"
#include "log.h"
#include "utils.hpp"

using namespace coro;

struct callback_handoff_state {
  std::atomic<bool> moving_result{false};
  std::atomic<bool> release_result{false};
  std::thread worker;
  bool completed = false;
};

struct handoff_value {
  callback_handoff_state* state;
  int generation = 0;

  explicit handoff_value(callback_handoff_state& value_state) : state(&value_state) {}
  handoff_value(const handoff_value&) = delete;

  handoff_value(handoff_value&& other) noexcept : state(other.state), generation(other.generation + 1) {
    // The first move passes std::function's argument to its target; the second
    // stores the callback result, after the callback has observed registration.
    if (generation == 2) {
      state->moving_result.store(true, std::memory_order_release);
      while (!state->release_result.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
  }
};

template <bool EXECUTOR_AWARE>
async<void> await_callback_during_handoff(callback_handoff_state& state) {
  auto registration = [&state](std::function<void(handoff_value)> callback) {
    state.worker = std::thread([&state, callback = std::move(callback)] {
      callback(handoff_value{state});
    });
    while (!state.moving_result.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  };
  auto awaiter = [&] {
    if constexpr (EXECUTOR_AWARE) {
      return callback_awaiter<handoff_value>([registration](executor* exec, std::function<void(handoff_value)> callback) {
        ASSERT(exec);
        registration(std::move(callback));
      });
    } else {
      return callback_awaiter<handoff_value>(registration);
    }
  }();
  auto value = co_await std::move(awaiter);
  ASSERT(value.state == &state);
  state.completed = true;
}

template <bool EXECUTOR_AWARE>
void test_callback_completion_during_handoff() {
  executor_poll executor;
  callback_handoff_state state;
  spawn(executor, await_callback_during_handoff<EXECUTOR_AWARE>(state));
  executor.poll();
  ASSERT(state.moving_result.load(std::memory_order_acquire));
  ASSERT(!state.completed);
  state.release_result.store(true, std::memory_order_release);
  state.worker.join();
  executor.poll();
  ASSERT(state.completed);
  check_coro_leak();
}

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

async<void> await_executor_aware_void_callbacks(int& completed_count) {
  co_await callback_awaiter<void>([](executor* exec, std::function<void()> callback) {
    ASSERT(exec);
    callback();
  });
  ++completed_count;
  co_await callback_awaiter<void>([](executor* exec, std::function<void()> callback) {
    exec->post(std::move(callback));
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

#ifndef CORO_DISABLE_EXCEPTION
struct extraction_state {
  int live = 0;
  bool fail_move = false;
};

struct throwing_callback_value {
  extraction_state* state;

  explicit throwing_callback_value(extraction_state& value_state) : state(&value_state) {
    ++state->live;
  }

  throwing_callback_value(const throwing_callback_value&) = delete;
  throwing_callback_value(throwing_callback_value&& other) : state(other.state) {
    if (state->fail_move) {
      throw std::runtime_error("callback result extraction failed");
    }
    ++state->live;
  }

  ~throwing_callback_value() {
    --state->live;
  }
};

template <bool EXECUTOR_AWARE>
async<void> await_throwing_callback_result() {
  extraction_state state;
  auto registration = [&state](std::function<void(throwing_callback_value)> callback) {
    callback(throwing_callback_value{state});
    state.fail_move = true;
  };
  bool caught = false;
  try {
    auto awaiter = [&] {
      if constexpr (EXECUTOR_AWARE) {
        return callback_awaiter<throwing_callback_value>([registration](executor* exec, std::function<void(throwing_callback_value)> callback) {
          ASSERT(exec);
          registration(std::move(callback));
        });
      } else {
        return callback_awaiter<throwing_callback_value>(registration);
      }
    }();
    [[maybe_unused]] auto value = co_await std::move(awaiter);
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "callback result extraction failed");
    caught = true;
  }
  ASSERT(caught);
  ASSERT(state.live == 0);
}
#endif

async<void> run_callback_tests(std::shared_ptr<callback_state> state, int& void_completed_count, executor*& observed_exec, int& async_result,
                               bool& async_callback_started, int& move_only_result, int& non_default_result) {
  co_await await_synchronous_callback(state);
  co_await await_synchronous_void_callback(void_completed_count);
  co_await await_executor_aware_void_callbacks(void_completed_count);
  co_await await_executor_aware_callback(observed_exec);
  co_await await_asynchronous_value_callback(async_result, async_callback_started);
  co_await await_move_only_callback_value(move_only_result);
  co_await await_non_default_move_only_callback_value(non_default_result);
#ifndef CORO_DISABLE_EXCEPTION
  co_await await_throwing_callback_result<false>();
  co_await await_throwing_callback_result<true>();
#endif
}

int main() {
  test_callback_completion_during_handoff<false>();
  test_callback_completion_during_handoff<true>();
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
  ASSERT(void_completed_count == 3);
  ASSERT(observed_exec == &loop);
  ASSERT(async_callback_started);
  ASSERT(async_result == 19);
  ASSERT(move_only_result == 23);
  ASSERT(non_default_result == 29);
  check_coro_leak();
  return 0;
}
