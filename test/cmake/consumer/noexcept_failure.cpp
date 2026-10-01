#define CORO_DEBUG_PROMISE_LEAK
void test_leak_log(const char*, ...);
#define CORO_DEBUG_LEAK_LOG test_leak_log
void test_lifecycle_log(const char*, ...);
#define CORO_DEBUG_LIFECYCLE test_lifecycle_log

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "assert_def.h"
#include "coro/channel.hpp"
#include "coro/condition_variable.hpp"
#include "coro/coro.hpp"
#include "log.h"
#include "utils.hpp"

namespace {
bool body_unwound = false;
bool fail_initial_log = false;
bool fail_leak_log = false;
const char* fail_safe_log_prefix = nullptr;
struct creation_failure {};

struct body_guard {
  ~body_guard() {
    body_unwound = true;
  }
};

template <typename T>
coro::async<T> task(bool fail) {
  body_guard guard;
  if (fail) {
    throw std::bad_alloc();
  }
  if constexpr (std::is_void_v<T>) {
    co_return;
  } else {
    co_return 42;
  }
}

template <typename T>
coro::async<T> initial_task([[maybe_unused]] std::shared_ptr<int> frame_token) {
  body_guard guard;
  if constexpr (std::is_void_v<T>) {
    co_return;
  } else {
    co_return 42;
  }
}

template <typename T>
void test_creation_failure(bool initial) {
  auto frame_token = std::make_shared<int>(0);
  fail_initial_log = initial;
  fail_leak_log = !initial;
  bool caught = false;
  try {
    auto value = initial_task<T>(frame_token);
  } catch (const creation_failure&) {
    caught = true;
  }
  ASSERT(caught);
  ASSERT(!fail_initial_log);
  ASSERT(!fail_leak_log);
  ASSERT(!body_unwound);
  ASSERT(frame_token.use_count() == 1);
  check_coro_leak();
  {
    auto value = initial_task<T>(frame_token);
    value.current_coro_handle_.resume();
    if constexpr (std::is_void_v<T>) {
      value.await_resume();
    } else {
      ASSERT(value.await_resume() == 42);
    }
  }
  ASSERT(body_unwound);
  ASSERT(frame_token.use_count() == 1);
  check_coro_leak();
}

template <typename T>
void test_nonthrowing_logs() {
  auto token = std::make_shared<int>(0);
  for (const char* prefix :
       {"promise: get_return_object:", "awaitable: new:", "awaitable: move(c):", "awaitable: move(=):", "awaitable: free:", "promise: final_suspend:",
        "final_awaitable: await_ready:", "final_awaitable: await_suspend:", "awaitable: await_ready:", "free promise:"}) {
    {
      if (std::strcmp(prefix, "promise: get_return_object:") == 0 || std::strcmp(prefix, "awaitable: new:") == 0) {
        fail_safe_log_prefix = prefix;
      }
      auto value = initial_task<T>(token);
      if (std::strcmp(prefix, "awaitable: move(c):") == 0) {
        fail_safe_log_prefix = prefix;
        auto moved = std::move(value);
        ASSERT(!value.current_coro_handle_);
        value = std::move(moved);
      } else if (std::strcmp(prefix, "awaitable: move(=):") == 0) {
        auto replacement = initial_task<T>(token);
        fail_safe_log_prefix = prefix;
        replacement = std::move(value);
        ASSERT(token.use_count() == 2);
        value = std::move(replacement);
      } else if (std::strcmp(prefix, "promise: get_return_object:") != 0 && std::strcmp(prefix, "awaitable: new:") != 0) {
        fail_safe_log_prefix = prefix;
      }
      value.current_coro_handle_.resume();
      ASSERT(value.await_ready());
      if constexpr (std::is_void_v<T>) {
        value.await_resume();
      } else {
        ASSERT(value.await_resume() == 42);
      }
    }
    ASSERT(!fail_safe_log_prefix);
    ASSERT(token.use_count() == 1);
    check_coro_leak();
  }
}

struct throwing_unlock_mutex {
  void unlock() {
    throw creation_failure{};
  }
};

coro::async<void> wait_for_unlock_failure(coro::condition_variable_st& cv, bool& caught) {
  throwing_unlock_mutex mutex;
  try {
    co_await cv.wait(mutex);
  } catch (const creation_failure&) {
    caught = true;
  }
}

void test_unlock_failure_cleanup() {
  coro::condition_variable_st cv;
  bool caught = false;
  {
    auto value = wait_for_unlock_failure(cv, caught);
    value.current_coro_handle_.resume();
    ASSERT(value.current_coro_handle_.done());
    value.await_resume();
  }
  ASSERT(caught);
  cv.notify_all();
  check_coro_leak();
}

struct broadcast_value {
  std::shared_ptr<int> token_;
  int* copies_;
  bool* fail_;

  broadcast_value(std::shared_ptr<int> token, int& copies, bool& fail) : token_(std::move(token)), copies_(&copies), fail_(&fail) {}
  broadcast_value(broadcast_value&&) noexcept = default;
  broadcast_value(const broadcast_value& other) : token_(other.token_), copies_(other.copies_), fail_(other.fail_) {
    if (++*copies_ == 3 && *fail_) {
      throw creation_failure{};
    }
  }
};

coro::async<void> receive_broadcast(coro::channel_st<broadcast_value>& channel, int& received) {
  auto value = co_await channel.recv();
  ASSERT(value && *value->token_ == 42);
  ++received;
}

coro::async<void> send_failed_broadcast(coro::channel_st<broadcast_value>& channel, const broadcast_value& value, bool& caught) {
  try {
    co_await channel.broadcast(value);
  } catch (const creation_failure&) {
    caught = true;
  }
}

void test_broadcast_failure_cleanup() {
  auto token = std::make_shared<int>(42);
  {
    int copies = 0;
    bool fail = true;
    int received = 0;
    coro::channel_st<broadcast_value> channel;
    auto first = receive_broadcast(channel, received);
    auto second = receive_broadcast(channel, received);
    first.current_coro_handle_.resume();
    second.current_coro_handle_.resume();
    broadcast_value value{token, copies, fail};
    bool caught = false;
    {
      // The third copy initializes the second receiver's result.
      auto sending = send_failed_broadcast(channel, value, caught);
      sending.current_coro_handle_.resume();
      sending.await_resume();
    }
    ASSERT(caught);
    ASSERT(received == 0);
    ASSERT(token.use_count() == 2);
    fail = false;
    copies = 0;
    {
      auto sending = send_failed_broadcast(channel, value, caught);
      sending.current_coro_handle_.resume();
      sending.await_resume();
    }
    ASSERT(received == 2);
    first.await_resume();
    second.await_resume();
  }
  ASSERT(token.use_count() == 1);
  check_coro_leak();
}

void fail_safe_log(const char* format) {
  if (fail_safe_log_prefix && std::strncmp(format, fail_safe_log_prefix, std::strlen(fail_safe_log_prefix)) == 0) {
    fail_safe_log_prefix = nullptr;
    throw creation_failure{};
  }
}

void expect_termination() {
  // Only the intended body failure should reach this handler, after local cleanup.
  auto error = std::current_exception();
  if (!error || !body_unwound) {
    std::_Exit(2);
  }
  try {
    std::rethrow_exception(error);
  } catch (const std::bad_alloc&) {
    // Termination leaves live coroutine frames; normal completion checks their release below.
    std::_Exit(0);
  } catch (...) {
    std::_Exit(3);
  }
}
}  // namespace

void test_leak_log(const char* format, ...) {
  fail_safe_log(format);
  if (fail_leak_log && std::strncmp(format, "new promise:", 12) == 0) {
    fail_leak_log = false;
    throw creation_failure{};
  }
}

void test_lifecycle_log(const char* format, ...) {
  fail_safe_log(format);
  if (fail_initial_log && std::strncmp(format, "promise: initial_suspend:", 25) == 0) {
    fail_initial_log = false;
    throw creation_failure{};
  }
}

int main(int argc, char** argv) {
  ASSERT(argc == 2);
  if (std::strcmp(argv[1], "initial_void") == 0) {
    test_creation_failure<void>(true);
    return 0;
  }
  if (std::strcmp(argv[1], "initial_value") == 0) {
    test_creation_failure<int>(true);
    return 0;
  }
  if (std::strcmp(argv[1], "leak_void") == 0) {
    test_creation_failure<void>(false);
    return 0;
  }
  if (std::strcmp(argv[1], "leak_value") == 0) {
    test_creation_failure<int>(false);
    return 0;
  }
  if (std::strcmp(argv[1], "normal") == 0) {
    test_nonthrowing_logs<void>();
    test_nonthrowing_logs<int>();
    test_unlock_failure_cleanup();
    test_broadcast_failure_cleanup();
    body_unwound = false;
    {
      auto empty = task<void>(false);
      empty.current_coro_handle_.resume();
      empty.await_resume();
      ASSERT(body_unwound);
      body_unwound = false;
      auto value = task<int>(false);
      value.current_coro_handle_.resume();
      ASSERT(value.await_resume() == 42);
      ASSERT(body_unwound);
    }
    check_coro_leak();
    return 0;
  }
  std::set_terminate(expect_termination);
  if (std::strcmp(argv[1], "void") == 0) {
    auto empty = task<void>(true);
    empty.current_coro_handle_.resume();
  } else {
    ASSERT(std::strcmp(argv[1], "value") == 0);
    auto value = task<int>(true);
    value.current_coro_handle_.resume();
  }
  // Reaching this point means unhandled_exception silently accepted the failure.
  return 1;
}
