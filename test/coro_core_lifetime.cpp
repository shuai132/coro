#define CORO_DEBUG_PROMISE_LEAK
#ifndef CORO_DISABLE_EXCEPTION
void test_leak_log(const char*, ...);
#define CORO_DEBUG_LEAK_LOG test_leak_log
#endif

#include <cstdint>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "coro/executor_poll.hpp"
#include "coro/time.hpp"
#include "coro/when.hpp"
#include "log.h"
#include "utils.hpp"

using namespace coro;

async<int> lazy_value() {
  co_return 42;
}

#ifndef CORO_DISABLE_EXCEPTION
bool fail_leak_log = false;

void test_leak_log(const char*, ...) {
  if (std::exchange(fail_leak_log, false)) {
    throw std::runtime_error("leak logger failed");
  }
}

void test_debug_log_failure() {
  fail_leak_log = true;
  bool caught = false;
  try {
    auto task = lazy_value();
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "leak logger failed");
    caught = true;
  }
  ASSERT(caught);
  ASSERT(debug_coro_promise::debug_coro_leak.empty());
  auto task = lazy_value();
  task.current_coro_handle_.resume();
  ASSERT(task.current_coro_handle_.done());
  ASSERT(task.await_resume() == 42);
}
#endif

async<void> child_without_executor(int& value) {
  value = 42;
  co_return;
}

async<void> spawn_local_without_executor(int& value) {
  co_await spawn_local(child_without_executor(value));
}

async<void> alignment_argument([[maybe_unused]] std::align_val_t alignment) {
  co_return;
}

struct frame_parameter {
  int* live;

  explicit frame_parameter(int& count) : live(&count) {
    ++*live;
  }
  frame_parameter(frame_parameter&& other) noexcept : live(std::exchange(other.live, nullptr)) {}
  frame_parameter(const frame_parameter&) = delete;
  ~frame_parameter() {
    if (live) {
      --*live;
    }
  }
};

template <typename T>
async<T> child_with_frame_parameter([[maybe_unused]] frame_parameter parameter, bool delayed, [[maybe_unused]] bool fail) {
  if (delayed) {
    co_await sleep(0ns);
  }
#ifndef CORO_DISABLE_EXCEPTION
  if (fail) {
    throw std::runtime_error("child frame failure");
  }
#endif
  if constexpr (!std::is_void_v<T>) {
    co_return 42;
  }
}

template <bool ANY, typename T>
async<void> await_child_frame_cleanup(bool delayed, bool fail, bool& completed) {
  int live = 0;
  bool caught = false;
#ifndef CORO_DISABLE_EXCEPTION
  try {
#endif
    if constexpr (ANY) {
      co_await when_any(child_with_frame_parameter<T>(frame_parameter{live}, delayed, fail));
    } else {
      co_await when_all(child_with_frame_parameter<T>(frame_parameter{live}, delayed, fail));
    }
#ifndef CORO_DISABLE_EXCEPTION
  } catch (const std::runtime_error& error) {
    ASSERT(std::string_view(error.what()) == "child frame failure");
    caught = true;
  }
#endif
  ASSERT(caught == fail);
  ASSERT(live == 0);
  completed = true;
}

template <bool ANY, typename T>
void test_child_frame_cleanup() {
  for (bool delayed : {false, true}) {
    for (bool fail : {false, true}) {
#ifdef CORO_DISABLE_EXCEPTION
      if (fail) {
        continue;
      }
#endif
      executor_poll executor;
      bool completed = false;
      spawn(executor, await_child_frame_cleanup<ANY, T>(delayed, fail, completed));
      while (!completed) {
        executor.poll();
      }
      check_coro_leak();
    }
  }
}

template <typename T>
void test_callback_child_frame_cleanup() {
  for (bool delayed : {false, true}) {
    for (bool fail : {false, true}) {
#ifdef CORO_DISABLE_EXCEPTION
      if (fail) {
        continue;
      }
#endif
      executor_poll executor;
      int live = 0;
      bool completed = false;
      auto completion = [&](auto... values) {
        ASSERT(!fail);
        ASSERT(live == 0);
        ((ASSERT(values == 42)), ...);
        completed = true;
      };
      auto task = child_with_frame_parameter<T>(frame_parameter{live}, delayed, fail);
#ifndef CORO_DISABLE_EXCEPTION
      task.detach_with_callback(executor, completion, [&](std::exception_ptr error) {
        ASSERT(fail);
        ASSERT(live == 0);
        bool caught = false;
        try {
          std::rethrow_exception(error);
        } catch (const std::runtime_error& failure) {
          ASSERT(std::string_view(failure.what()) == "child frame failure");
          caught = true;
        }
        ASSERT(caught);
        completed = true;
      });
#else
      task.detach_with_callback(executor, completion);
#endif
      while (!completed) {
        executor.poll();
      }
      check_coro_leak();
    }
  }
}

struct tracked_result {
  int* live;

  explicit tracked_result(int& count) : live(&count) {
    ++*live;
  }
  tracked_result(tracked_result&& other) noexcept : live(other.live) {
    ++*live;
  }
  tracked_result(const tracked_result&) = delete;
  ~tracked_result() {
    --*live;
  }
};

async<tracked_result> child_with_tracked_result(int& live, bool delayed) {
  if (delayed) {
    co_await sleep(0ns);
  }
  co_return tracked_result{live};
}

async<void> gated_loser(std::function<void()>& release, bool& completed) {
  co_await callback_awaiter<void>([&release](std::function<void()> callback) {
    release = std::move(callback);
  });
  completed = true;
}

template <bool ANY>
async<void> await_result_cleanup(bool delayed, bool& completed) {
  int live = 0;
  {
    if constexpr (ANY) {
      auto result = co_await when_any(child_with_tracked_result(live, delayed));
      ASSERT(result.index == 0);
    } else {
      auto result = co_await when_all(child_with_tracked_result(live, delayed));
      ASSERT(result.live == &live);
    }
  }
  ASSERT(live == 0);
  if constexpr (ANY) {
    std::function<void()> release;
    bool loser_completed = false;
    {
      auto result = co_await when_any(child_with_tracked_result(live, delayed), gated_loser(release, loser_completed));
      ASSERT(result.index == 0);
      ASSERT(!loser_completed);
    }
    ASSERT(live == 0);
    ASSERT(release);
    release();
    ASSERT(loser_completed);
  } else {
    {
      auto results = co_await when_all(child_with_tracked_result(live, delayed), child_with_tracked_result(live, delayed));
      ASSERT(std::get<0>(results).live == &live);
      ASSERT(std::get<1>(results).live == &live);
    }
    ASSERT(live == 0);
#ifndef CORO_DISABLE_EXCEPTION
    bool caught = false;
    try {
      co_await when_all(child_with_tracked_result(live, false), child_with_frame_parameter<void>(frame_parameter{live}, delayed, true));
    } catch (const std::runtime_error&) {
      caught = true;
    }
    ASSERT(caught);
    ASSERT(live == 0);
#endif
  }
  completed = true;
}

template <bool ANY>
void test_combined_result_cleanup() {
  for (bool delayed : {false, true}) {
    executor_poll executor;
    bool completed = false;
    spawn(executor, await_result_cleanup<ANY>(delayed, completed));
    while (!completed) {
      executor.poll();
    }
    check_coro_leak();
  }
}

#ifndef CORO_DISABLE_EXCEPTION
struct tracked_failure : std::exception {
  int* live;

  explicit tracked_failure(int& count) : live(&count) {
    ++*live;
  }
  tracked_failure(const tracked_failure& other) : live(other.live) {
    ++*live;
  }
  ~tracked_failure() override {
    --*live;
  }
};

template <typename T>
async<T> child_with_tracked_failure(int& live, bool delayed) {
  if (delayed) {
    co_await sleep(0ns);
  }
  throw tracked_failure{live};
  if constexpr (!std::is_void_v<T>) {
    co_return 42;
  }
}

template <bool ANY, typename T>
async<void> await_exception_cleanup(bool delayed, bool& completed) {
  int live = 0;
  bool caught = false;
  try {
    if constexpr (ANY) {
      co_await when_any(child_with_tracked_failure<T>(live, delayed));
    } else {
      co_await when_all(child_with_tracked_failure<T>(live, delayed));
    }
  } catch (const tracked_failure& error) {
    ASSERT(error.live == &live);
    caught = true;
  }
  ASSERT(caught);
  ASSERT(live == 0);
  caught = false;
  if constexpr (ANY) {
    std::function<void()> release;
    bool loser_completed = false;
    try {
      co_await when_any(child_with_tracked_failure<T>(live, delayed), gated_loser(release, loser_completed));
    } catch (const tracked_failure&) {
      caught = true;
    }
    ASSERT(caught);
    ASSERT(live == 0);
    ASSERT(release);
    ASSERT(!loser_completed);
    release();
    ASSERT(loser_completed);
  } else {
    try {
      co_await when_all(child_with_tracked_failure<T>(live, false), child_with_tracked_failure<T>(live, delayed));
    } catch (const tracked_failure&) {
      caught = true;
    }
    ASSERT(caught);
    ASSERT(live == 0);
  }
  completed = true;
}

template <bool ANY, typename T>
void test_combined_exception_cleanup() {
  for (bool delayed : {false, true}) {
    executor_poll executor;
    bool completed = false;
    spawn(executor, await_exception_cleanup<ANY, T>(delayed, completed));
    while (!completed) {
      executor.poll();
    }
    check_coro_leak();
  }
}
#endif

void test_alignment_argument() {
  auto task = alignment_argument(std::align_val_t{0});
  task.current_coro_handle_.resume();
  ASSERT(task.current_coro_handle_.done());
}

#ifdef CORO_TEST_ALIGNED_COROUTINE_ALLOCATION
struct alignas(256) aligned_result {
  int value;
};

async<aligned_result> aligned_value() {
  co_return aligned_result{42};
}

void test_aligned_promise() {
  std::vector<async<aligned_result>> tasks;
  for (int index = 0; index < 128; ++index) {
    tasks.push_back(aligned_value());
  }
  for (auto& task : tasks) {
    ASSERT(reinterpret_cast<std::uintptr_t>(&task.current_coro_handle_.promise()) % alignof(awaitable_promise<aligned_result>) == 0);
    task.current_coro_handle_.resume();
    ASSERT(task.current_coro_handle_.done());
    ASSERT(task.await_resume().value == 42);
  }
}
#endif

int main() {
  test_callback_child_frame_cleanup<int>();
  test_callback_child_frame_cleanup<void>();
  test_child_frame_cleanup<false, int>();
  test_child_frame_cleanup<false, void>();
  test_child_frame_cleanup<true, int>();
  test_child_frame_cleanup<true, void>();
  test_combined_result_cleanup<false>();
  test_combined_result_cleanup<true>();
#ifndef CORO_DISABLE_EXCEPTION
  test_combined_exception_cleanup<false, int>();
  test_combined_exception_cleanup<false, void>();
  test_combined_exception_cleanup<true, int>();
  test_combined_exception_cleanup<true, void>();
#endif
#ifndef CORO_DISABLE_EXCEPTION
  test_debug_log_failure();
#endif
  test_alignment_argument();
#ifdef CORO_TEST_ALIGNED_COROUTINE_ALLOCATION
  test_aligned_promise();
#endif
  ASSERT(reinterpret_cast<std::uintptr_t>(&L_O_G_NS_MUTEX::mutex()) % alignof(std::mutex) == 0);
  ASSERT(debug_coro_promise::debug_coro_leak.empty());

  {
    auto task = lazy_value();
    (void)task;
  }

  ASSERT(debug_coro_promise::debug_coro_leak.empty());

  int value = 0;
  {
    executor_poll executor;
    spawn(executor, child_without_executor(value));
    ASSERT(debug_coro_promise::debug_coro_leak.size() == 1);
  }
  ASSERT(value == 0);
  ASSERT(debug_coro_promise::debug_coro_leak.empty());

  {
    auto task = spawn_local_without_executor(value);
    task.current_coro_handle_.resume();
    ASSERT(task.current_coro_handle_.done());
  }

  ASSERT(value == 42);
  ASSERT(debug_coro_promise::debug_coro_leak.empty());

  value = 0;
  {
    // Retain the lazy spawn across the full-expression that creates its child.
    auto task = spawn_local(child_without_executor(value));
    ASSERT(debug_coro_promise::debug_coro_leak.size() == 2);
    task.current_coro_handle_.resume();
    ASSERT(task.current_coro_handle_.done());
  }
  ASSERT(value == 42);
  ASSERT(debug_coro_promise::debug_coro_leak.empty());

  value = 0;
  {
    auto task = spawn_local(child_without_executor(value));
    (void)task;
  }
  ASSERT(value == 0);
  ASSERT(debug_coro_promise::debug_coro_leak.empty());

  {
    auto child = child_without_executor(value);
    auto task = spawn_local(child);
    ASSERT(!child.current_coro_handle_);
    task.current_coro_handle_.resume();
    ASSERT(task.current_coro_handle_.done());
  }
  ASSERT(value == 42);
  ASSERT(debug_coro_promise::debug_coro_leak.empty());

  value = 0;
  {
    executor_poll executor;
    auto task = spawn_local(child_without_executor(value)).bind_executor(executor);
    task.current_coro_handle_.resume();
    ASSERT(task.current_coro_handle_.done());
    ASSERT(value == 0);
    executor.poll();
    ASSERT(value == 42);
  }
  ASSERT(debug_coro_promise::debug_coro_leak.empty());
  return 0;
}
