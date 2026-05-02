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
#include "log.h"

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
