#pragma once

#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "executor.hpp"

/// options
// #define CORO_DISABLE_EXCEPTION

#ifndef CORO_DEBUG_LEAK_LOG
#define CORO_DEBUG_LEAK_LOG(...) (void)(0)
#endif

#ifndef CORO_DEBUG_LIFECYCLE
#define CORO_DEBUG_LIFECYCLE(...) (void)(0)
#endif

/// compiler check and debug config
#if defined(CORO_DEBUG_PROMISE_LEAK)
#include <cstdio>
#include <mutex>
#include <unordered_set>
struct debug_coro_promise {
  inline static std::unordered_set<void*> debug_coro_leak;
  inline static std::mutex debug_coro_leak_mutex;

  static void dump() {
    std::lock_guard<std::mutex> lock(debug_coro_leak_mutex);
    CORO_DEBUG_LEAK_LOG("debug: debug_coro_leak.size: %zu", debug_coro_leak.size());
  }

  void* operator new(std::size_t size) {
    void* ptr = std::malloc(size);
    {
      std::lock_guard<std::mutex> lock(debug_coro_leak_mutex);
      debug_coro_leak.insert(ptr);
      CORO_DEBUG_LEAK_LOG("new: %p, size: %zu, num: %zu", ptr, size, debug_coro_leak.size());
    }
    return ptr;
  }

  void operator delete(void* ptr, [[maybe_unused]] std::size_t size) {
    {
      std::lock_guard<std::mutex> lock(debug_coro_leak_mutex);
      debug_coro_leak.erase(ptr);
      CORO_DEBUG_LEAK_LOG("free: %p, size: %zu, num: %zu", ptr, size, debug_coro_leak.size());
    }
    std::free(ptr);
  }
};
#else
struct debug_coro_promise {};
#endif

/// coroutine types
namespace coro {
template <typename T>
struct awaitable;

template <typename T>
struct awaitable_promise;

template <typename T>
struct callback_awaiter;
}  // namespace coro

namespace coro {

template <typename T>
struct awaitable_promise_value {
  template <typename R>
  void return_value(R&& val) noexcept {
#ifndef CORO_DISABLE_EXCEPTION
    value_.template emplace<T>(std::forward<R>(val));
#else
    value_.emplace(std::forward<R>(val));
#endif
  }

  void unhandled_exception() noexcept {
#ifndef CORO_DISABLE_EXCEPTION
    value_.template emplace<std::exception_ptr>(std::current_exception());
#endif
  }

  T get_value() const {
#ifndef CORO_DISABLE_EXCEPTION
    if (std::holds_alternative<std::exception_ptr>(value_)) {
      std::rethrow_exception(std::get<std::exception_ptr>(value_));
    }
    return std::get<T>(value_);
#else
    return value_.value();
#endif
  }

#ifndef CORO_DISABLE_EXCEPTION
  std::variant<std::exception_ptr, T> value_{nullptr};
#else
  std::optional<T> value_;
#endif
};

template <>
struct awaitable_promise_value<void> {
  void return_void() noexcept {}

  void unhandled_exception() noexcept {
#ifndef CORO_DISABLE_EXCEPTION
    exception_ = std::current_exception();
#endif
  }

  void get_value() const {
#ifndef CORO_DISABLE_EXCEPTION
    if (exception_) {
      std::rethrow_exception(exception_);
    }
#endif
  }

#ifndef CORO_DISABLE_EXCEPTION
  std::exception_ptr exception_{nullptr};
#endif
};

template <typename T>
struct final_awaitable {
  awaitable_promise<T>* self;

  bool await_ready() noexcept {
    CORO_DEBUG_LIFECYCLE("final_awaitable: await_ready: p: %p, p.h: %p", self, self->parent_handle_.address());
    return false;
  }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<awaitable_promise<T>> h) noexcept {
    CORO_DEBUG_LIFECYCLE("final_awaitable: await_suspend: p: %p, h: %p, p.h: %p", self, h.address(), h.promise().parent_handle_.address());
    CORO_DEBUG_LIFECYCLE("done: %d", h.done());
    // h.promise() is self
    if (h.promise().parent_handle_) {
      return h.promise().parent_handle_;
    } else {
      if (h.done() && !h.promise().awaitable_) {
        h.destroy();
      }
      return std::noop_coroutine();
    }
  }

  void await_resume() noexcept {
    CORO_DEBUG_LIFECYCLE("final_awaitable: await_resume: %p, p.h: %p", self, self->parent_handle_.address());
    self->get_value();
  }
};

template <typename T>
struct awaitable_promise : awaitable_promise_value<T>, debug_coro_promise {
  awaitable<T> get_return_object();

  std::suspend_always initial_suspend() {
    CORO_DEBUG_LIFECYCLE("promise: initial_suspend: p: %p, p.h: %p", this, parent_handle_.address());
    return {};
  }

  final_awaitable<T> final_suspend() noexcept {
    CORO_DEBUG_LIFECYCLE("promise: final_suspend: p: %p, p.h: %p", this, parent_handle_.address());
    return final_awaitable<T>{this};
  }

  std::coroutine_handle<> parent_handle_{};
  executor* executor_ = nullptr;       // from bind_executor or inherit from caller
  awaitable<T>* awaitable_ = nullptr;  // have awaitable lived or detached
};

template <typename T>
struct awaitable {
  using promise_type = awaitable_promise<T>;

  explicit awaitable(std::coroutine_handle<promise_type> h) : current_coro_handle_(h) {
    CORO_DEBUG_LIFECYCLE("awaitable: new: %p, h: %p", this, h.address());
    h.promise().awaitable_ = this;
  }
  ~awaitable() {
    CORO_DEBUG_LIFECYCLE("awaitable: free: %p, h: %p, done: %s", this, current_coro_handle_ ? current_coro_handle_.address() : nullptr,
                         current_coro_handle_ ? current_coro_handle_.done() ? "yes" : "no" : "null");
    if (current_coro_handle_) {
      if (current_coro_handle_.done()) {
        current_coro_handle_.destroy();
      } else {
        current_coro_handle_.promise().awaitable_ = nullptr;
      }
    }
  }

  /// disable copy
  awaitable(const awaitable&) = delete;
  awaitable(awaitable&) = delete;
  awaitable& operator=(const awaitable&) = delete;
  awaitable& operator=(awaitable&) = delete;

  /// enable move
  awaitable(awaitable&& other) noexcept : current_coro_handle_(other.current_coro_handle_) {
    CORO_DEBUG_LIFECYCLE("awaitable: move(c): %p to %p, h: %p", &other, this, current_coro_handle_.address());
    other.current_coro_handle_ = nullptr;
  }
  awaitable& operator=(awaitable&& other) noexcept {
    CORO_DEBUG_LIFECYCLE("awaitable: move(=): %p to %p, h: %p", &other, this, current_coro_handle_.address());
    if (this != &other) {
      if (current_coro_handle_) current_coro_handle_.destroy();
      current_coro_handle_ = other.current_coro_handle_;
      other.current_coro_handle_ = nullptr;
    }
    return *this;
  }

  /// co_await
  bool await_ready() const noexcept {
    CORO_DEBUG_LIFECYCLE("awaitable: await_ready: %p, h: %p", this, current_coro_handle_.address());
    return false;
  }

  template <typename Promise>
  auto await_suspend(std::coroutine_handle<Promise> h) {
    CORO_DEBUG_LIFECYCLE("awaitable: await_suspend: %p, h: %p, p.h: %p", this, current_coro_handle_.address(), h.address());
    // use bind executor or inherit from caller
    if (!current_coro_handle_.promise().executor_) {
      current_coro_handle_.promise().executor_ = h.promise().executor_;
    }
    current_coro_handle_.promise().parent_handle_ = h;
    return current_coro_handle_;
  }

  T await_resume() {
    CORO_DEBUG_LIFECYCLE("awaitable: await_resume: %p, h: %p", this, current_coro_handle_.address());
    return current_coro_handle_.promise().get_value();
  }

  auto bind_executor(executor& exec) {
    current_coro_handle_.promise().executor_ = &exec;
    return std::move(*this);
  }

  void detach(auto& executor) {
    auto* exec_to_use = current_coro_handle_.promise().executor_ ? current_coro_handle_.promise().executor_ : &executor;
    exec_to_use->dispatch([coro = std::make_shared<awaitable<T>>(std::move(*this)), exec_to_use]() mutable {
      coro->current_coro_handle_.promise().executor_ = exec_to_use;
      CORO_DEBUG_LIFECYCLE("awaitable: resume begin: %p, h: %p", coro.get(), coro->current_coro_handle_.address());
      coro->current_coro_handle_.resume();
      CORO_DEBUG_LIFECYCLE("awaitable: resume end: %p, h: %p", coro.get(), coro->current_coro_handle_.address());
    });
  }

  template <typename Function>
  auto with_callback(Function completion_handler, std::function<void(std::exception_ptr)> exception_handler = nullptr) {
    auto coro = [](awaitable<T> lazy, auto completion_handler, [[maybe_unused]] auto exception_handler) mutable -> awaitable<void> {
#ifndef CORO_DISABLE_EXCEPTION
      try {
#endif
        if constexpr (std::is_void_v<T>) {
          co_await std::move(lazy);
          completion_handler();
        } else {
          completion_handler(co_await std::move(lazy));
        }
#ifndef CORO_DISABLE_EXCEPTION
      } catch (...) {
        if (exception_handler) exception_handler(std::current_exception());
      }
#endif
    }(std::move(*this), std::move(completion_handler), std::move(exception_handler));
    return coro;
  }

  template <typename Function>
  auto detach_with_callback(auto& executor, Function completion_handler, std::function<void(std::exception_ptr)> exception_handler = nullptr) {
    auto coro = with_callback(std::move(completion_handler), std::move(exception_handler));
    return coro.detach(executor);  // launched here
  }

  std::coroutine_handle<promise_type> current_coro_handle_;
};

template <typename T>
awaitable<T> awaitable_promise<T>::get_return_object() {
  auto handle = std::coroutine_handle<awaitable_promise<T>>::from_promise(*this);
  CORO_DEBUG_LIFECYCLE("promise: get_return_object: p: %p, h: %p", this, handle.address());
  return awaitable<T>{handle};
}

/// callback_awaiter
namespace detail {

template <typename T>
struct callback_awaiter_base {
  using callback_function_no_executor = std::function<void(std::function<void(T)>)>;
  using callback_function_with_executor = std::function<void(executor*, std::function<void(T)>)>;

  T await_resume() noexcept {
    return std::move(result_);
  }

  T result_;
};

template <>
struct callback_awaiter_base<void> {
  using callback_function_no_executor = std::function<void(std::function<void()>)>;
  using callback_function_with_executor = std::function<void(executor*, std::function<void()>)>;

  void await_resume() noexcept {}
};

}  // namespace detail

template <typename T>
struct callback_awaiter : detail::callback_awaiter_base<T> {
  using callback_function_no_executor = detail::callback_awaiter_base<T>::callback_function_no_executor;
  using callback_function_with_executor = detail::callback_awaiter_base<T>::callback_function_with_executor;
  std::variant<callback_function_no_executor, callback_function_with_executor> callback_function_;

  explicit callback_awaiter(callback_function_no_executor callback) : callback_function_(std::move(callback)) {}
  explicit callback_awaiter(callback_function_with_executor callback) : callback_function_(std::move(callback)) {}
  callback_awaiter(callback_awaiter&&) = default;

  bool await_ready() const noexcept {
    return false;
  }

  template <typename Promise>
  void await_suspend(std::coroutine_handle<Promise> handle) {
    auto executor = handle.promise().executor_;
    switch (callback_function_.index()) {
      case 0: {
        auto& func = std::get<0>(callback_function_);
        if constexpr (std::is_void_v<T>) {
          func([handle, executor]() {
            executor->dispatch([handle] {
              handle.resume();
            });
          });
        } else {
          func([handle, this, executor](T value) {
            executor->dispatch([handle, this, value = std::move(value)]() mutable {
              this->result_ = std::move(value);
              handle.resume();
            });
          });
        }
      } break;
      case 1: {
        auto& func = std::get<1>(callback_function_);
        if constexpr (std::is_void_v<T>) {
          func(executor, [handle, executor]() {
            executor->dispatch([handle] {
              handle.resume();
            });
          });
        } else {
          func(executor, [handle, this, executor](T value) {
            executor->dispatch([handle, this, value = std::move(value)]() mutable {
              this->result_ = std::move(value);
              handle.resume();
            });
          });
        }
      } break;
    }
  }
};

/// current_executor_awaiter
namespace detail {

struct current_executor_awaiter {
  bool await_ready() const noexcept {
    return false;
  }

  template <typename Promise>
  bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
    exec_ = h.promise().executor_;
    return false;
  }

  executor* await_resume() const noexcept {
    return exec_;
  }

 private:
  executor* exec_ = nullptr;
};

}  // namespace detail

detail::current_executor_awaiter current_executor() {
  return detail::current_executor_awaiter{};
}

template <typename T>
using async = awaitable<T>;

template <typename T>
void co_spawn(executor& executor, T&& coro) {
  coro.detach(executor);
}

}  // namespace coro
