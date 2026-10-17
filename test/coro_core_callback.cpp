#define CORO_DEBUG_PROMISE_LEAK

#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "coro/executor_loop.hpp"
#include "coro/executor_poll.hpp"
#include "coro/time.hpp"
#include "log.h"
#include "utils.hpp"

using namespace coro;

template <bool EXECUTOR_AWARE, bool VALUE>
async<void> await_callback_with_bound_executor(std::function<void()>& complete, bool& completed) {
  auto registration = [&complete](auto callback) {
    complete = [callback = std::move(callback)]() mutable {
      if constexpr (VALUE) {
        callback(42);
      } else {
        callback();
      }
    };
  };
  using result_type = std::conditional_t<VALUE, int, void>;
  auto awaiter = [&] {
    if constexpr (EXECUTOR_AWARE) {
      return callback_awaiter<result_type>([registration](executor*, auto callback) {
        registration(std::move(callback));
      });
    } else {
      return callback_awaiter<result_type>(registration);
    }
  }();
  if constexpr (VALUE) {
    ASSERT(co_await std::move(awaiter) == 42);
  } else {
    co_await std::move(awaiter);
  }
  completed = true;
}

template <bool EXECUTOR_AWARE, bool VALUE>
void test_callback_uses_prestart_binding() {
  executor_poll original;
  executor_poll rebound;
  bool completed = false;
  std::function<void()> complete;
  {
    auto task = await_callback_with_bound_executor<EXECUTOR_AWARE, VALUE>(complete, completed).bind_executor(original);
    ASSERT(!task.current_coro_handle_.promise().started_);
    auto rebound_task = task.bind_executor(rebound);
    ASSERT(rebound_task.get_executor() == &rebound);
    ASSERT(!rebound_task.current_coro_handle_.promise().started_);
    rebound_task.current_coro_handle_.resume();
    ASSERT(complete);
    ASSERT(!completed);
    complete();
    ASSERT(!completed);
    original.poll();
    ASSERT(!completed);
    rebound.poll();
    ASSERT(completed);
    complete = nullptr;
  }
  check_coro_leak();
}

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
    // Pause the move into result storage while registration is still active.
    if (generation == 1) {
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

struct recording_timer_executor : executor_poll {
  uint64_t last_delay = 0;

  void post_delayed_ns(std::function<void()> callback, uint64_t delay_ns) override {
    last_delay = delay_ns;
    post(std::move(callback));
  }
};

async<void> await_sleep_durations(recording_timer_executor& exec, bool& completed) {
  co_await sleep(-1ms);
  ASSERT(exec.last_delay == 0);
  co_await std::chrono::seconds::min();
  ASSERT(exec.last_delay == 0);
  co_await sleep(0ns);
  ASSERT(exec.last_delay == 0);
  co_await sleep(1ms);
  ASSERT(exec.last_delay == 1000000);
  co_await sleep(std::chrono::duration<double, std::milli>{1.5});
  ASSERT(exec.last_delay == 1500000);
  co_await delay(2);
  ASSERT(exec.last_delay == 2000000);
  co_await delay(std::numeric_limits<uint64_t>::max());
  ASSERT(exec.last_delay == static_cast<uint64_t>(std::chrono::nanoseconds::max().count()));
  co_await delay(static_cast<uint64_t>(std::chrono::nanoseconds::max().count()) / 1000000 + 1);
  ASSERT(exec.last_delay == static_cast<uint64_t>(std::chrono::nanoseconds::max().count()));
  completed = true;
}

async<void> await_large_sleep_durations(recording_timer_executor& exec, bool& completed) {
  constexpr auto period_limit = std::numeric_limits<std::intmax_t>::max();
  using enormous_seconds = std::chrono::duration<int64_t, std::ratio<period_limit>>;
  using below_seconds = std::chrono::duration<uint64_t, std::ratio<period_limit - 1, period_limit>>;
  using above_seconds = std::chrono::duration<uint64_t, std::ratio<period_limit, period_limit - 1>>;
  co_await sleep(enormous_seconds{0});
  ASSERT(exec.last_delay == 0);
  co_await sleep(enormous_seconds{-1});
  ASSERT(exec.last_delay == 0);
  co_await sleep(enormous_seconds{1});
  ASSERT(exec.last_delay == static_cast<uint64_t>(std::chrono::nanoseconds::max().count()));
  co_await sleep(below_seconds{1});
  ASSERT(exec.last_delay == 999999999);
  co_await sleep(below_seconds{2});
  ASSERT(exec.last_delay == 1999999999);
  co_await sleep(above_seconds{1});
  ASSERT(exec.last_delay == 1000000000);
  co_await sleep(above_seconds{2});
  ASSERT(exec.last_delay == 2000000000);
  co_await sleep(below_seconds{std::numeric_limits<uint64_t>::max()});
  ASSERT(exec.last_delay == static_cast<uint64_t>(std::chrono::nanoseconds::max().count()));
  co_await sleep(below_seconds{9223372036ULL});
  ASSERT(exec.last_delay == 9223372035999999999ULL);
  co_await sleep(below_seconds{9223372037ULL});
  ASSERT(exec.last_delay == static_cast<uint64_t>(std::chrono::nanoseconds::max().count()));
  co_await sleep(std::chrono::duration<uint64_t, std::ratio<period_limit, 1000000001>>{1});
  ASSERT(exec.last_delay == 9223372027631403779ULL);
  co_await sleep(std::chrono::duration<double, above_seconds::period>{0.5});
  ASSERT(exec.last_delay == 500000000);
  co_await sleep(std::chrono::duration<double, enormous_seconds::period>{1.0});
  ASSERT(exec.last_delay == static_cast<uint64_t>(std::chrono::nanoseconds::max().count()));
  constexpr auto max_ns = static_cast<uint64_t>(std::chrono::nanoseconds::max().count());
  co_await sleep(std::chrono::seconds::max());
  ASSERT(exec.last_delay == max_ns);
  co_await std::chrono::hours::max();
  ASSERT(exec.last_delay == max_ns);
  co_await sleep(std::chrono::duration<uint64_t, std::nano>{std::numeric_limits<uint64_t>::max()});
  ASSERT(exec.last_delay == max_ns);
  co_await sleep(std::chrono::duration<double>{std::numeric_limits<double>::infinity()});
  ASSERT(exec.last_delay == max_ns);
  co_await sleep(std::chrono::duration<double>{std::numeric_limits<double>::quiet_NaN()});
  ASSERT(exec.last_delay == 0);
  // Preserve integral precision beyond the exact range of a double.
  co_await sleep(std::chrono::nanoseconds{9007199254740993LL});
  ASSERT(exec.last_delay == 9007199254740993ULL);
  co_await sleep(std::chrono::duration<uint64_t, std::ratio<2, 3>>{10000000000ULL});
  ASSERT(exec.last_delay == 6666666666666666666ULL);
  // The intermediate product overflows uint64_t, but the final quotient fits.
  co_await sleep(std::chrono::duration<uint64_t, std::ratio<1, 9223372036854775783LL>>{std::numeric_limits<uint64_t>::max()});
  ASSERT(exec.last_delay == 2000000000ULL);
  co_await sleep(std::chrono::duration<uint64_t, std::ratio<1, 9223372036854775783LL>>{9223372036854775782ULL});
  ASSERT(exec.last_delay == 999999999ULL);
  co_await sleep(std::chrono::duration<uint64_t, std::ratio<1, 9223372036854775783LL>>{4611686018427387891ULL});
  ASSERT(exec.last_delay == 499999999ULL);
  co_await sleep(std::chrono::duration<int64_t, std::pico>{std::numeric_limits<int64_t>::max()});
  ASSERT(exec.last_delay == max_ns / 1000);
  co_await sleep(std::chrono::nanoseconds{std::chrono::nanoseconds::max().count() - 1});
  ASSERT(exec.last_delay == max_ns - 1);
  for (double seconds : {0.3, 1.5, 12.3456789}) {
    auto duration = std::chrono::duration<double>{seconds};
    co_await sleep(duration);
    ASSERT(exec.last_delay == static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()));
  }
  auto float_duration = std::chrono::duration<float>{0.3f};
  co_await sleep(float_duration);
  ASSERT(exec.last_delay == static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(float_duration).count()));
  auto long_double_duration = std::chrono::duration<long double>{0.3L};
  co_await sleep(long_double_duration);
  ASSERT(exec.last_delay == static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(long_double_duration).count()));
  completed = true;
}

void test_large_sleep_duration_conversion() {
  recording_timer_executor exec;
  bool completed = false;
  spawn(exec, await_large_sleep_durations(exec, completed));
  for (int turn = 0; turn < 32 && !completed; ++turn) {
    exec.poll();
  }
  ASSERT(completed);
  check_coro_leak();
}

#ifdef __SIZEOF_INT128__
template <typename REP>
async<void> await_wide_sleep_durations(recording_timer_executor& exec, bool& completed) {
  constexpr auto max_ns = static_cast<uint64_t>(std::chrono::nanoseconds::max().count());
  constexpr REP huge = REP{1} << 126;
  co_await sleep(std::chrono::duration<REP>{0});
  ASSERT(exec.last_delay == 0);
  co_await sleep(std::chrono::duration<REP>{1});
  ASSERT(exec.last_delay == 1000000000);
  if constexpr (std::numeric_limits<REP>::is_signed) {
    co_await sleep(std::chrono::duration<REP>{-huge});
    ASSERT(exec.last_delay == 0);
  }
  co_await sleep(std::chrono::duration<REP>{huge});
  ASSERT(exec.last_delay == max_ns);
  co_await sleep(std::chrono::duration<REP, std::nano>{REP{max_ns} + 1});
  ASSERT(exec.last_delay == max_ns);
  co_await sleep(std::chrono::duration<REP, std::pico>{REP{max_ns} * 1000 - 1});
  ASSERT(exec.last_delay == max_ns - 1);
  co_await sleep(std::chrono::duration<REP, std::pico>{REP{max_ns} * 1000 + 999});
  ASSERT(exec.last_delay == max_ns);
  constexpr auto period_limit = std::numeric_limits<std::intmax_t>::max();
  co_await sleep(std::chrono::duration<REP, std::ratio<period_limit>>{huge});
  ASSERT(exec.last_delay == max_ns);
  co_await sleep(std::chrono::duration<REP, std::ratio<1, period_limit>>{REP{1} << 70});
  ASSERT(exec.last_delay == 128000000000ULL);
  completed = true;
}

template <typename REP>
void test_wide_sleep_duration_conversion() {
  recording_timer_executor exec;
  bool completed = false;
  spawn(exec, await_wide_sleep_durations<REP>(exec, completed));
  for (int turn = 0; turn < 16 && !completed; ++turn) {
    exec.poll();
  }
  ASSERT(completed);
  check_coro_leak();
}
#endif

void test_sleep_duration_conversion() {
  recording_timer_executor exec;
  bool completed = false;
  spawn(exec, await_sleep_durations(exec, completed));
  for (int turn = 0; turn < 16 && !completed; ++turn) {
    exec.poll();
  }
  ASSERT(completed);
  check_coro_leak();
}

struct detached_completion_state {
  std::atomic<bool> release{false};
  std::atomic<bool> finished{false};
  std::thread worker;
};

struct concurrent_completion_executor : executor {
  detached_completion_state& state;

  explicit concurrent_completion_executor(detached_completion_state& completion) : state(completion) {}

  void dispatch(std::function<void()> function) override {
    function();
    state.release.store(true, std::memory_order_release);
    while (!state.finished.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
  }

  void post(std::function<void()>) override {
    ASSERT(false);
  }

  void post_delayed_ns(std::function<void()>, uint64_t) override {
    ASSERT(false);
  }
};

async<void> finish_while_detach_returns(detached_completion_state& state) {
  co_await callback_awaiter<void>([&state](std::function<void()> callback) {
    state.worker = std::thread([&state, callback = std::move(callback)] {
      while (!state.release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      callback();
    });
  });
  state.finished.store(true, std::memory_order_relaxed);
}

void test_concurrent_detach_completion() {
  for (int iteration = 0; iteration < 128; ++iteration) {
    detached_completion_state state;
    concurrent_completion_executor exec{state};
    spawn(exec, finish_while_detach_returns(state));
    state.worker.join();
    ASSERT(state.finished.load(std::memory_order_relaxed));
  }
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

static_assert(std::is_nothrow_move_constructible_v<callback_awaiter<void>>);
static_assert(std::is_nothrow_move_constructible_v<callback_awaiter<int>>);
static_assert(std::is_nothrow_move_constructible_v<callback_awaiter<std::unique_ptr<int>>>);
static_assert(std::is_nothrow_move_constructible_v<callback_awaiter<non_default_move_only_value>>);
#ifndef CORO_DISABLE_EXCEPTION
static_assert(!noexcept(std::declval<callback_awaiter<int>&>().await_resume()));
#else
static_assert(noexcept(std::declval<callback_awaiter<int>&>().await_resume()));
#endif

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
struct throwing_copy_callback_value {
  int* live_;

  explicit throwing_copy_callback_value(int& live) noexcept : live_(&live) {
    ++*live_;
  }
  throwing_copy_callback_value(const throwing_copy_callback_value&) {
    throw std::runtime_error("callback copy failed");
  }
  throwing_copy_callback_value(throwing_copy_callback_value&& other) noexcept : live_(std::exchange(other.live_, nullptr)) {}
  ~throwing_copy_callback_value() {
    if (live_) {
      --*live_;
    }
  }
};

void test_nothrow_move_callback_storage_failure() {
  int live = 0;
  {
    throwing_copy_callback_value value{live};
    detail::callback_awaiter_base<throwing_copy_callback_value> awaiter;
    awaiter.set_result(value);
    bool caught = false;
    try {
      (void)awaiter.await_resume();
    } catch (const std::runtime_error& error) {
      ASSERT(std::string_view(error.what()) == "callback copy failed");
      caught = true;
    }
    ASSERT(caught);
    ASSERT(live == 1);
    awaiter.set_result(std::move(value));
    auto result = awaiter.await_resume();
    ASSERT(result.live_ == &live);
  }
  ASSERT(live == 0);
}

struct callback_storage_state {
  int live = 0;
  bool escaped = false;
  bool caught = false;
  bool fail_storage = true;
  int fail_generation = 1;
};

struct throwing_storage_value {
  callback_storage_state* state;
  int generation = 0;

  explicit throwing_storage_value(callback_storage_state& value_state) : state(&value_state) {
    ++state->live;
  }

  throwing_storage_value(const throwing_storage_value&) = delete;
  throwing_storage_value(throwing_storage_value&& other) : state(other.state), generation(other.generation + 1) {
    if (generation == state->fail_generation && state->fail_storage) {
      throw std::runtime_error("callback result storage failed");
    }
    ++state->live;
  }

  ~throwing_storage_value() {
    --state->live;
  }
};

static_assert(!std::is_nothrow_move_constructible_v<callback_awaiter<throwing_storage_value>>);

template <bool EXECUTOR_AWARE>
async<void> await_throwing_callback_storage(executor& exec, callback_storage_state& state, bool asynchronous) {
  auto registration = [&exec, &state, asynchronous](std::function<void(throwing_storage_value)> callback) {
    auto invoke = [&state, callback = std::move(callback)] {
      try {
        callback(throwing_storage_value{state});
      } catch (...) {
        state.escaped = true;
      }
    };
    if (asynchronous) {
      exec.post(std::move(invoke));
    } else {
      invoke();
    }
  };
  try {
    auto awaiter = [&] {
      if constexpr (EXECUTOR_AWARE) {
        return callback_awaiter<throwing_storage_value>([registration](executor* exec, std::function<void(throwing_storage_value)> callback) {
          ASSERT(exec);
          registration(std::move(callback));
        });
      } else {
        return callback_awaiter<throwing_storage_value>(registration);
      }
    }();
    [[maybe_unused]] auto value = co_await std::move(awaiter);
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "callback result storage failed");
    state.caught = true;
  }
}

template <bool EXECUTOR_AWARE>
void test_callback_storage_failure(bool asynchronous, int fail_generation) {
  executor_poll exec;
  callback_storage_state state;
  state.fail_generation = fail_generation;
  bool completed = false;
  {
    auto task = await_throwing_callback_storage<EXECUTOR_AWARE>(exec, state, asynchronous).bind_executor(exec);
    task.current_coro_handle_.resume();
    exec.poll();
    completed = task.current_coro_handle_.done();
  }
  ASSERT(state.live == 0);
  check_coro_leak();
  ASSERT(completed);
  ASSERT(state.caught);
  ASSERT(!state.escaped);
}

template <bool EXECUTOR_AWARE>
async<void> await_reused_callback(executor& exec, bool asynchronous) {
  callback_storage_state state;
  auto registration = [&exec, &state, asynchronous](std::function<void(throwing_storage_value)> callback) {
    auto invoke = [&state, callback = std::move(callback)] {
      callback(throwing_storage_value{state});
    };
    if (asynchronous) {
      exec.post(std::move(invoke));
    } else {
      invoke();
    }
  };
  {
    auto awaiter = [&] {
      if constexpr (EXECUTOR_AWARE) {
        return callback_awaiter<throwing_storage_value>([registration](executor* exec, std::function<void(throwing_storage_value)> callback) {
          ASSERT(exec);
          registration(std::move(callback));
        });
      } else {
        return callback_awaiter<throwing_storage_value>(registration);
      }
    }();
    for (bool fail : {true, false, true, false}) {
      state.fail_storage = fail;
      bool caught = false;
      try {
        auto value = co_await awaiter;
        ASSERT(value.state == &state);
      } catch (const std::runtime_error& error) {
        ASSERT(std::string_view(error.what()) == "callback result storage failed");
        caught = true;
      }
      ASSERT(caught == fail);
    }
  }
  // Inline dispatch can resume us before the last callback argument is destroyed.
  co_await sleep(0ns);
  ASSERT(state.live == 0);
}

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
  auto* exec = co_await current_executor();
  for (bool asynchronous : {false, true}) {
    co_await await_reused_callback<false>(*exec, asynchronous);
    co_await await_reused_callback<true>(*exec, asynchronous);
  }
#endif
}

int main() {
  test_callback_uses_prestart_binding<false, false>();
  test_callback_uses_prestart_binding<false, true>();
  test_callback_uses_prestart_binding<true, false>();
  test_callback_uses_prestart_binding<true, true>();
#ifndef CORO_DISABLE_EXCEPTION
  test_nothrow_move_callback_storage_failure();
#endif
#ifdef __SIZEOF_INT128__
  test_wide_sleep_duration_conversion<__int128>();
  test_wide_sleep_duration_conversion<unsigned __int128>();
#endif
  test_large_sleep_duration_conversion();
#ifndef CORO_DISABLE_EXCEPTION
  for (bool asynchronous : {false, true}) {
    for (int fail_generation : {1, 2}) {
      test_callback_storage_failure<false>(asynchronous, fail_generation);
      test_callback_storage_failure<true>(asynchronous, fail_generation);
    }
  }
#endif
  test_concurrent_detach_completion();
  test_sleep_duration_conversion();
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
