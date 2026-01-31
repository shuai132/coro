#pragma once

/// ASIO Adaptor for coro library
/// This header provides interoperability between coro::async<T> and asio::awaitable<T>
///
/// Features:
/// 1. asio_executor: Adapts asio::io_context to coro::executor interface
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
#include <optional>
#include <type_traits>
#include <utility>

#include "../coro.hpp"
#include "../executor.hpp"

namespace coro {

/// asio_executor: Adapts asio::io_context to coro::executor interface
/// This allows coro coroutines to run on an asio event loop
class asio_executor : public executor {
 public:
  explicit asio_executor(asio::io_context& io_context) : io_context_(io_context) {}

  void dispatch(std::function<void()> fn) override {
    asio::dispatch(io_context_, std::move(fn));
  }

  void post(std::function<void()> fn) override {
    asio::post(io_context_, std::move(fn));
  }

  void post_delayed_ns(std::function<void()> fn, uint64_t delay_ns) override {
    auto timer = std::make_shared<asio::steady_timer>(io_context_);
    timer->expires_after(std::chrono::nanoseconds(delay_ns));
    timer->async_wait([timer, fn = std::move(fn)](const asio::error_code& ec) mutable {
      timer = nullptr;
      if (!ec) {
        fn();
      }
    });
  }

  void stop() override {
    io_context_.stop();
  }

  asio::io_context& get_io_context() {
    return io_context_;
  }

 private:
  asio::io_context& io_context_;
};

namespace detail {

/// Helper to get io_context from executor (works with asio_executor)
inline asio::io_context* get_io_context_from_executor(executor* exec) {
  if (auto* asio_exec = dynamic_cast<asio_executor*>(exec)) {
    return &asio_exec->get_io_context();
  }
  return nullptr;
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

  bool await_ready() const noexcept {
    return false;
  }

  template <typename Promise>
  void await_suspend(std::coroutine_handle<Promise> handle) {
    auto* exec = handle.promise().executor_;
    assert(exec && "executor must be set");

    auto* io_ctx = get_io_context_from_executor(exec);
    assert(io_ctx && "asio_executor required for await_asio");

    auto state = state_;  // Copy shared_ptr for the lambda

    // Spawn the asio awaitable and resume when complete
    asio::co_spawn(*io_ctx, std::move(asio_awaitable_), [state, handle, io_ctx](std::exception_ptr ep, T value) {
      // If io_context is stopped, don't resume the coroutine as it may have been
      // destroyed or its captured references may be invalid
      if (io_ctx->stopped()) {
        return;
      }
#ifndef CORO_DISABLE_EXCEPTION
      if (ep) {
        state->exception = ep;
      } else {
        state->result = std::move(value);
      }
#else
      state->result = std::move(value);
      (void)ep;
#endif
      // Use asio::post to schedule the resume. Check stopped() again inside
      // the posted handler since io_context may be stopped between now and
      // when the handler executes.
      asio::post(*io_ctx, [handle, io_ctx]() {
        if (!io_ctx->stopped()) {
          handle.resume();
        }
      });
    });
  }

  T await_resume() {
#ifndef CORO_DISABLE_EXCEPTION
    if (state_->exception) {
      std::rethrow_exception(state_->exception);
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

    auto* io_ctx = get_io_context_from_executor(exec);
    assert(io_ctx && "asio_executor required for await_asio");

#ifndef CORO_DISABLE_EXCEPTION
    auto state = state_;  // Copy shared_ptr for the lambda

    asio::co_spawn(*io_ctx, std::move(asio_awaitable_), [state, handle, io_ctx](std::exception_ptr ep) {
      // If io_context is stopped, don't resume the coroutine as it may have been
      // destroyed or its captured references may be invalid
      if (io_ctx->stopped()) {
        return;
      }
      state->exception = ep;
      // Use asio::post to schedule the resume. Check stopped() again inside
      // the posted handler since io_context may be stopped between now and
      // when the handler executes.
      asio::post(*io_ctx, [handle, io_ctx]() {
        if (!io_ctx->stopped()) {
          handle.resume();
        }
      });
    });
#else
    asio::co_spawn(*io_ctx, std::move(asio_awaitable_), [handle, io_ctx](std::exception_ptr) {
      if (io_ctx->stopped()) {
        return;
      }
      asio::post(*io_ctx, [handle, io_ctx]() {
        if (!io_ctx->stopped()) {
          handle.resume();
        }
      });
    });
#endif
  }

  void await_resume() {
#ifndef CORO_DISABLE_EXCEPTION
    if (state_->exception) {
      std::rethrow_exception(state_->exception);
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
  auto& io_ctx = static_cast<asio::io_context&>(executor.context());

  // Use a shared state to communicate between callbacks
  // Also stores asio_executor to keep it alive during coro execution
  struct state {
    asio_executor exec;
    std::optional<T> result;
#ifndef CORO_DISABLE_EXCEPTION
    std::exception_ptr exception;
#endif
    explicit state(asio::io_context& ctx) : exec(ctx) {}
  };
  auto st = std::make_shared<state>(io_ctx);

  // Create a deferred timer that we'll use to signal completion
  auto signal_timer = std::make_shared<asio::steady_timer>(io_ctx);
  signal_timer->expires_at(asio::steady_timer::time_point::max());

  aw.detach_with_callback(
      st->exec,
      [st, signal_timer](T value) {
        st->result = std::move(value);
        signal_timer->cancel();
      },
      [st, signal_timer]([[maybe_unused]] std::exception_ptr ep) {
#ifndef CORO_DISABLE_EXCEPTION
        st->exception = ep;
#endif
        signal_timer->cancel();
      });

  // Wait for the signal (timer cancel)
  asio::error_code ec;
  co_await signal_timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));

#ifndef CORO_DISABLE_EXCEPTION
  if (st->exception) {
    std::rethrow_exception(st->exception);
  }
#endif
  co_return std::move(st->result.value());
}

/// Specialization for void
template <>
[[nodiscard]] inline asio::awaitable<void> await_coro(async<void> aw) {
  auto executor = co_await asio::this_coro::executor;
  auto& io_ctx = static_cast<asio::io_context&>(executor.context());

  // Create a deferred timer that we'll use to signal completion
  auto signal_timer = std::make_shared<asio::steady_timer>(io_ctx);
  signal_timer->expires_at(asio::steady_timer::time_point::max());

#ifndef CORO_DISABLE_EXCEPTION
  // Use a shared state to communicate exception between callbacks
  // Also stores asio_executor to keep it alive during coro execution
  struct state {
    asio_executor exec;
    std::exception_ptr exception;
    explicit state(asio::io_context& ctx) : exec(ctx) {}
  };
  auto st = std::make_shared<state>(io_ctx);

  aw.detach_with_callback(
      st->exec,
      [signal_timer]() {
        signal_timer->cancel();
      },
      [st, signal_timer](const std::exception_ptr& ep) {
        st->exception = ep;
        signal_timer->cancel();
      });

  // Wait for the signal (timer cancel)
  asio::error_code ec;
  co_await signal_timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));

  if (st->exception) {
    std::rethrow_exception(st->exception);
  }
#else
  // Without exceptions, asio_executor can be stored in shared state
  struct state {
    asio_executor exec;
    explicit state(asio::io_context& ctx) : exec(ctx) {}
  };
  auto st = std::make_shared<state>(io_ctx);

  aw.detach_with_callback(
      st->exec,
      [signal_timer]() {
        signal_timer->cancel();
      },
      [signal_timer](std::exception_ptr) {
        signal_timer->cancel();
      });

  // Wait for the signal (timer cancel)
  asio::error_code ec;
  co_await signal_timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
#endif
}

}  // namespace coro
