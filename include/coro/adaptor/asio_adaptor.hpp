#pragma once

/// ASIO Adaptor for coro library
/// This header provides interoperability between coro::async<T> and asio::awaitable<T>
///
/// Features:
/// 1. asio_executor: Adapts asio::any_io_executor to coro::executor interface
/// 2. await_asio(): Allows coro coroutines to co_await asio::awaitable<T>
/// 3. await_coro(): Allows asio coroutines to co_await coro::async<T>
///
/// Usage:
///   // In coro coroutine, await asio awaitable:
///   coro::async<void> my_coro_task() {
///       auto result = co_await coro::await_asio(some_asio_awaitable());
///   }
///
///   // In asio coroutine, await coro async:
///   asio::awaitable<void> my_asio_task() {
///       auto result = co_await coro::await_coro(some_coro_async());
///   }

#include <asio.hpp>
#include <cassert>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include "../coro.hpp"
#include "../executor.hpp"
#include "coro/detail/deadline.hpp"

namespace coro {

/// asio_executor: Adapts asio::any_io_executor to coro::executor interface
/// This allows coro coroutines to run on an asio event loop
/// Non-owning: keep the context and this adapter alive until all bound tasks and callbacks finish.
class asio_executor : public executor {
 public:
  explicit asio_executor(asio::io_context& io_context) : asio_executor(io_context.get_executor()) {}

  explicit asio_executor(asio::any_io_executor exec) : asio_exec_(std::move(exec)) {}

  void dispatch(std::function<void()> fn) override {
    // Preserve executor properties: only unwrap the exact default io_context executor type.
    if (auto* concrete = asio_exec_.target<asio::io_context::executor_type>()) {
      asio::dispatch(*concrete, std::move(fn));
    } else {
      asio::dispatch(asio_exec_, std::move(fn));
    }
  }

  void post(std::function<void()> fn) override {
    if (auto* concrete = asio_exec_.target<asio::io_context::executor_type>()) {
      asio::post(*concrete, std::move(fn));
    } else {
      asio::post(asio_exec_, std::move(fn));
    }
  }

  void post_delayed_ns(std::function<void()> fn, uint64_t delay_ns) override {
    auto timer = std::make_shared<asio::steady_timer>(asio_exec_);
    timer->expires_at(detail::delayed_deadline(delay_ns));
    timer->async_wait([timer, fn = std::move(fn)](const asio::error_code& ec) mutable {
      timer = nullptr;
      if (!ec) {
        fn();
      }
    });
  }

  const asio::any_io_executor& get_asio_executor() const noexcept {
    return asio_exec_;
  }

 private:
  asio::any_io_executor asio_exec_;
};

namespace detail {

// use_awaitable initiates lazily, so the lock must cover async_wait's actual
// initiation rather than only the creation of its returned awaitable.
template <typename State>
auto wait_coro_signal(std::shared_ptr<State> state, std::shared_ptr<asio::steady_timer> timer, asio::error_code& error) {
  auto token = asio::redirect_error(asio::use_awaitable, error);
  return asio::async_initiate<decltype(token), void(asio::error_code)>(
      [state = std::move(state), timer = std::move(timer)](auto handler) {
        std::lock_guard<std::mutex> lock(state->signal_mutex_);
        timer->async_wait(std::move(handler));
      },
      token);
}

// Release the child's frame and moved-from result before making completion visible.
template <typename T, typename STATE>
async<void> complete_coro_signal(async<T> task, std::shared_ptr<STATE> state, std::shared_ptr<asio::steady_timer> timer) {
#ifndef CORO_DISABLE_EXCEPTION
  std::exception_ptr failure;
  try {
#endif
    if constexpr (std::is_void_v<T>) {
      co_await async<T>{std::move(task)};
    } else {
      std::optional<T> result{std::in_place, co_await async<T>{std::move(task)}};
      // Only this producer accesses result until completed is published under the mutex.
      state->result.emplace(std::move(*result));
    }
#ifndef CORO_DISABLE_EXCEPTION
  } catch (...) {
    failure = std::current_exception();
  }
#endif
  std::lock_guard<std::mutex> lock(state->signal_mutex_);
#ifndef CORO_DISABLE_EXCEPTION
  state->exception = std::move(failure);
#endif
  state->completed = true;
  timer->expires_at(asio::steady_timer::time_point::min());
}

/// Awaiter for awaiting asio::awaitable<T> inside coro::async<T>
template <typename T>
struct asio_awaitable_awaiter {
  // Shared state to safely communicate between asio callback and coro coroutine
  struct shared_state {
    std::optional<T> result;
#ifndef CORO_DISABLE_EXCEPTION
    std::exception_ptr exception;
#endif
  };

  asio::awaitable<T> asio_awaitable_;
  std::shared_ptr<shared_state> state_;

  explicit asio_awaitable_awaiter(asio::awaitable<T> aw) : asio_awaitable_(std::move(aw)), state_(std::make_shared<shared_state>()) {}

  static asio::awaitable<void> store_result(asio::awaitable<T> task, std::shared_ptr<shared_state> state) {
#ifndef CORO_DISABLE_EXCEPTION
    try {
#endif
      state->result.emplace(co_await std::move(task));
#ifndef CORO_DISABLE_EXCEPTION
    } catch (...) {
      // co_spawn otherwise keeps its own exception copy while invoking completion.
      state->exception = std::current_exception();
    }
#endif
  }

  bool await_ready() const noexcept {
    return false;
  }

  template <typename Promise>
  void await_suspend(std::coroutine_handle<Promise> handle) {
    auto* exec = handle.promise().executor_;
    assert(exec && "executor must be set");

    auto* asio_exec = dynamic_cast<asio_executor*>(exec);
    assert(asio_exec && "asio_executor required for await_asio");
    auto state = state_;  // Copy shared_ptr for the lambda

    // Spawn the asio awaitable and resume when complete
    asio::co_spawn(asio_exec->get_asio_executor(), store_result(std::move(asio_awaitable_), state), [state, handle, exec]([[maybe_unused]] auto ep) {
#ifndef CORO_DISABLE_EXCEPTION
      if (ep) {
        state->exception = std::move(ep);
      }
#endif
      // Preserve the executor's strand and retain completion across io_context stop/restart.
      exec->post([handle]() {
        handle.resume();
      });
    });
  }

  T await_resume() {
    // The completion callback can retain state after another worker resumes us.
    struct result_cleanup {
      std::optional<T>& result_;
      ~result_cleanup() {
        result_.reset();
      }
    } cleanup{state_->result};
#ifndef CORO_DISABLE_EXCEPTION
    if (auto error = std::exchange(state_->exception, {})) {
      std::rethrow_exception(error);
    }
#endif
    return std::move(state_->result.value());
  }
};

/// Specialization for void
template <>
struct asio_awaitable_awaiter<void> {
#ifndef CORO_DISABLE_EXCEPTION
  // Shared state to safely communicate between asio callback and coro coroutine
  struct shared_state {
    std::exception_ptr exception;
  };
#endif

  asio::awaitable<void> asio_awaitable_;
#ifndef CORO_DISABLE_EXCEPTION
  std::shared_ptr<shared_state> state_;

  explicit asio_awaitable_awaiter(asio::awaitable<void> aw) : asio_awaitable_(std::move(aw)), state_(std::make_shared<shared_state>()) {}

  static asio::awaitable<void> store_exception(asio::awaitable<void> task, std::shared_ptr<shared_state> state) {
    try {
      co_await std::move(task);
    } catch (...) {
      // End the catch before co_spawn invokes completion on another worker.
      state->exception = std::current_exception();
    }
  }
#else
  explicit asio_awaitable_awaiter(asio::awaitable<void> aw) : asio_awaitable_(std::move(aw)) {}
#endif

  bool await_ready() const noexcept {
    return false;
  }

  template <typename Promise>
  void await_suspend(std::coroutine_handle<Promise> handle) {
    auto* exec = handle.promise().executor_;
    assert(exec && "executor must be set");

    auto* asio_exec = dynamic_cast<asio_executor*>(exec);
    assert(asio_exec && "asio_executor required for await_asio");
#ifndef CORO_DISABLE_EXCEPTION
    auto state = state_;  // Copy shared_ptr for the lambda

    asio::co_spawn(asio_exec->get_asio_executor(), store_exception(std::move(asio_awaitable_), state), [state, handle, exec](std::exception_ptr ep) {
      if (ep) {
        state->exception = std::move(ep);
      }
      // Preserve the executor's strand and retain completion across io_context stop/restart.
      exec->post([handle]() {
        handle.resume();
      });
    });
#else
    asio::co_spawn(asio_exec->get_asio_executor(), std::move(asio_awaitable_), [handle, exec](auto) {
      exec->post([handle]() {
        handle.resume();
      });
    });
#endif
  }

  void await_resume() {
#ifndef CORO_DISABLE_EXCEPTION
    if (auto error = std::exchange(state_->exception, {})) {
      std::rethrow_exception(error);
    }
#endif
  }
};

}  // namespace detail

/// await_asio: Allows coro coroutines to co_await asio::awaitable<T>
/// Usage: auto result = co_await await_asio(some_asio_awaitable());
template <typename T>
[[nodiscard]] auto await_asio(asio::awaitable<T> aw) {
  return detail::asio_awaitable_awaiter<T>(std::move(aw));
}

/// await_coro: Allows asio coroutines to co_await coro::async<T>
/// Returns an asio::awaitable<T> that wraps the coro::async<T>
/// Usage: auto result = co_await await_coro(some_coro_async());
template <typename T>
[[nodiscard]] asio::awaitable<T> await_coro(async<T> aw) {
  auto executor = co_await asio::this_coro::executor;

  // Use a shared state to communicate between callbacks
  // Also stores asio_executor to keep it alive during coro execution
  struct state : asio_executor {
    std::mutex signal_mutex_;
    std::optional<T> result;
    bool completed = false;
#ifndef CORO_DISABLE_EXCEPTION
    std::exception_ptr exception;
#endif
    explicit state(asio::any_io_executor exec) : asio_executor(std::move(exec)) {}
  };
  auto st = std::make_shared<state>(std::move(executor));

  // Create a deferred timer that we'll use to signal completion
  auto signal_timer = std::make_shared<asio::steady_timer>(st->get_asio_executor());
  signal_timer->expires_at(asio::steady_timer::time_point::max());

  detail::complete_coro_signal(std::move(aw), st, signal_timer).detach(*st);

  // Expiry persists even if completion happened before async_wait was registered.
  // Wait for the completion signal
  asio::error_code ec;
  co_await detail::wait_coro_signal(st, signal_timer, ec);
  {
    std::lock_guard<std::mutex> lock(st->signal_mutex_);
#ifndef CORO_DISABLE_EXCEPTION
    if (!st->completed) {
      throw asio::system_error(ec);
    }
    if (auto error = std::exchange(st->exception, {})) {
      std::rethrow_exception(error);
    }
#endif
  }
  struct result_cleanup {
    std::optional<T>& result_;
    ~result_cleanup() {
      result_.reset();
    }
  } cleanup{st->result};
  co_return std::move(st->result.value());
}

/// Specialization for void
template <>
[[nodiscard]] inline asio::awaitable<void> await_coro(async<void> aw) {
  auto executor = co_await asio::this_coro::executor;

  // Create a deferred timer that we'll use to signal completion
  auto signal_timer = std::make_shared<asio::steady_timer>(executor);
  signal_timer->expires_at(asio::steady_timer::time_point::max());

  struct state : asio_executor {
    std::mutex signal_mutex_;
    bool completed = false;
#ifndef CORO_DISABLE_EXCEPTION
    std::exception_ptr exception;
#endif
    explicit state(asio::any_io_executor exec) : asio_executor(std::move(exec)) {}
  };
  auto st = std::make_shared<state>(std::move(executor));

  detail::complete_coro_signal(std::move(aw), st, signal_timer).detach(*st);

  // Expiry persists even if completion happened before async_wait was registered.
  asio::error_code ec;
  co_await detail::wait_coro_signal(st, signal_timer, ec);
  std::lock_guard<std::mutex> lock(st->signal_mutex_);
#ifndef CORO_DISABLE_EXCEPTION
  if (!st->completed) {
    throw asio::system_error(ec);
  }
  if (auto error = std::exchange(st->exception, {})) {
    std::rethrow_exception(error);
  }
#endif
}

}  // namespace coro
