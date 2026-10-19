#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "coro/detail/async_debug.hpp"
#include "coro/detail/resume.hpp"
#include "coro/detail/shared_ptr.hpp"
#include "executor.hpp"

#ifdef CORO_ENABLE_FRAME_CACHE
#include "coro/detail/frame_cache.hpp"
#endif

#ifdef CORO_ENABLE_LOCAL_STORAGE
#include "coro_local_map.hpp"
#endif

/// options
// #define CORO_DISABLE_EXCEPTION
// #define CORO_ENABLE_LOCAL_STORAGE

// Debug logging hooks must not throw, including during coroutine creation and explicit dumps.
#ifndef CORO_DEBUG_LEAK_LOG
#define CORO_DEBUG_LEAK_LOG(...) (void)(0)
#endif

#ifndef CORO_DEBUG_LIFECYCLE
#define CORO_DEBUG_LIFECYCLE(...) (void)(0)
#endif

/// compiler check and debug config
#if defined(CORO_DEBUG_PROMISE_LEAK)
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <unordered_set>
struct debug_coro_promise {
 private:
  struct leak_state_t {
    std::mutex mutex_;
    std::unordered_set<void*> promises_;
  };

  static leak_state_t& get_leak_state() {
    // Other translation units can create coroutines during both static initialization and teardown.
    // Retain this debug-only registry for the process lifetime instead of depending on global destruction order.
    static auto* state = new leak_state_t;
    return *state;
  }

 public:
  inline static auto& debug_coro_leak = get_leak_state().promises_;
  inline static auto& debug_coro_leak_mutex = get_leak_state().mutex_;

  static void dump() {
    auto& state = get_leak_state();
    [[maybe_unused]] std::size_t count;
    {
      std::lock_guard<std::mutex> lock(state.mutex_);
      count = state.promises_.size();
    }
    CORO_DEBUG_LEAK_LOG("debug: debug_coro_leak.size: %zu", count);
  }

  debug_coro_promise()
#ifdef CORO_DISABLE_EXCEPTION
      noexcept
#endif
  {
    auto& state = get_leak_state();
    [[maybe_unused]] std::size_t count;
    {
      std::lock_guard<std::mutex> lock(state.mutex_);
      state.promises_.insert(this);
      count = state.promises_.size();
    }
    // User logging may create another coroutine; never call it while holding the tracking lock.
    CORO_DEBUG_LEAK_LOG("new promise: %p, num: %zu", this, count);
  }

  ~debug_coro_promise() {
    auto& state = get_leak_state();
    [[maybe_unused]] std::size_t count;
    {
      std::lock_guard<std::mutex> lock(state.mutex_);
      state.promises_.erase(this);
      count = state.promises_.size();
    }
    CORO_DEBUG_LEAK_LOG("free promise: %p, num: %zu", this, count);
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

namespace detail {

// Keep submission and rollback out of the same-executor transfer path, shared by all result types.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
[[gnu::noinline]]
#endif
    inline void post_child(executor* exec, std::coroutine_handle<> child, [[maybe_unused]] std::coroutine_handle<>& parent_handle,
                           [[maybe_unused]] executor*& parent_executor) {
#ifndef CORO_DISABLE_EXCEPTION
  try {
#endif
    exec->post(resume_task{child});
#ifndef CORO_DISABLE_EXCEPTION
  } catch (...) {
    // A rejected start keeps the child owned by its awaitable, with no suspended parent to resume.
    parent_handle = {};
    parent_executor = nullptr;
    throw;
  }
#endif
}

}  // namespace detail

template <typename T>
struct awaitable_promise_value {
  template <typename R = T>
  void return_value(R&& val) noexcept(std::is_nothrow_constructible_v<T, R&&>) {
#ifndef CORO_DISABLE_EXCEPTION
    value_.template emplace<2>(std::forward<R>(val));
#else
    value_.emplace(std::forward<R>(val));
#endif
  }

  void unhandled_exception() noexcept {
#ifndef CORO_DISABLE_EXCEPTION
    value_.template emplace<1>(std::current_exception());
#else
    std::terminate();
#endif
  }

  T get_value() {
#ifndef CORO_DISABLE_EXCEPTION
    if (value_.index() == 1) {
      std::rethrow_exception(std::get<1>(value_));
    }
    return std::move(std::get<2>(value_));
#else
    return std::move(value_.value());
#endif
  }

#ifndef CORO_DISABLE_EXCEPTION
  std::variant<std::monostate, std::exception_ptr, T> value_;
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
#else
    std::terminate();
#endif
  }

  void get_value() {
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
      if (h.promise().parent_executor_ != h.promise().executor_) {
        // Distinct executors can share a thread; dispatch would recurse on each completed child.
        // An unbound parent continues on the child's executor, also through its queue.
        auto* exec = h.promise().parent_executor_ ? h.promise().parent_executor_ : h.promise().executor_;
        detail::post_resume_via(exec, h.promise().parent_handle_);
        return std::noop_coroutine();
      }
      return h.promise().parent_handle_;
    } else {
      if (!h.promise().awaitable_) {
        h.destroy();
      }
      return std::noop_coroutine();
    }
  }

  void await_resume() noexcept {
    CORO_DEBUG_LIFECYCLE("final_awaitable: await_resume: %p, p.h: %p", self, self->parent_handle_.address());
  }
};

template <typename T>
struct awaitable_promise : awaitable_promise_value<T>, debug_coro_promise {
#ifdef CORO_ENABLE_FRAME_CACHE
  static void* operator new(std::size_t size) {
    return detail::allocate_frame(size, alignof(awaitable_promise));
  }

  // Coroutine parameters are lvalues: do not mistake an align_val_t parameter for allocation alignment.
  template <typename ALIGNMENT>
    requires std::is_same_v<ALIGNMENT, std::align_val_t>
  static void* operator new(std::size_t size, ALIGNMENT&& alignment) {
    auto requested = static_cast<std::size_t>(alignment);
    return detail::allocate_frame(size, requested < alignof(awaitable_promise) ? alignof(awaitable_promise) : requested);
  }

  static void operator delete(void* frame, std::size_t size) noexcept {
    detail::release_frame(frame, size, alignof(awaitable_promise));
  }

  static void operator delete(void* frame, std::size_t size, std::align_val_t alignment) noexcept {
    auto requested = static_cast<std::size_t>(alignment);
    detail::release_frame(frame, size, requested < alignof(awaitable_promise) ? alignof(awaitable_promise) : requested);
  }
#endif

#ifdef CORO_DEBUG_ASYNC_STACK
  awaitable<T> get_return_object(detail::async_debug_location location = detail::async_debug_location::current()) noexcept;
  detail::async_debug_node debug_node_;
#else
  awaitable<T> get_return_object() noexcept;
#endif

  struct initial_awaitable {
    awaitable_promise* self_;

    bool await_ready() const noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {
      self_->started_ = true;
    }
  };

  initial_awaitable initial_suspend()
#if !defined(CORO_DEBUG_ASYNC_STACK) || defined(CORO_DISABLE_EXCEPTION)
      noexcept
#endif
  {
#ifdef CORO_DEBUG_ASYNC_STACK
#ifndef CORO_DISABLE_EXCEPTION
    try {
#endif
      debug_node_.register_frame();
#ifndef CORO_DISABLE_EXCEPTION
    } catch (...) {
      // Registration can allocate or lock. Creation failure destroys the frame and its return object.
      awaitable_->current_coro_handle_ = nullptr;
      throw;
    }
#endif
#endif
    CORO_DEBUG_LIFECYCLE("promise: initial_suspend: p: %p, p.h: %p", this, parent_handle_.address());
    return {this};
  }

  final_awaitable<T> final_suspend() noexcept {
#ifdef CORO_DEBUG_ASYNC_STACK
    debug_node_.complete();
#endif
    CORO_DEBUG_LIFECYCLE("promise: final_suspend: p: %p, p.h: %p", this, parent_handle_.address());
    return final_awaitable<T>{this};
  }

  std::coroutine_handle<> parent_handle_{};
  executor* parent_executor_ = nullptr;
  executor* executor_ = nullptr;       // from bind_executor or inherit from caller
  awaitable<T>* awaitable_ = nullptr;  // have awaitable lived or detached
  bool started_ = false;

#ifdef CORO_ENABLE_LOCAL_STORAGE
  bool owns_local_storage_ = false;
  // Read-only children share the inherited map; only local writes need a new layer.
  detail::shared_ptr<detail::coro_local_map> local_storage_;

  void ensure_local_storage() {
    if (!owns_local_storage_) {
      auto storage = detail::make_shared<detail::coro_local_map>();
      storage->parent = local_storage_;
      local_storage_ = std::move(storage);
      owns_local_storage_ = true;
    }
  }
#endif
};

template <typename T>
struct awaitable {
  using promise_type = awaitable_promise<T>;

  explicit awaitable(std::coroutine_handle<promise_type> h) noexcept : current_coro_handle_(h) {
    CORO_DEBUG_LIFECYCLE("awaitable: new: %p, h: %p", this, h.address());
    h.promise().awaitable_ = this;
  }
  ~awaitable() {
    CORO_DEBUG_LIFECYCLE("awaitable: free: %p, h: %p, done: %s", this, current_coro_handle_ ? current_coro_handle_.address() : nullptr,
                         current_coro_handle_ ? current_coro_handle_.done() ? "yes" : "no" : "null");
    if (current_coro_handle_) {
      if (current_coro_handle_.done() || !current_coro_handle_.promise().started_) {
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
    if (current_coro_handle_) {
      current_coro_handle_.promise().awaitable_ = this;
    }
    other.current_coro_handle_ = nullptr;
  }
  awaitable& operator=(awaitable&& other) noexcept {
    CORO_DEBUG_LIFECYCLE("awaitable: move(=): %p to %p, h: %p", &other, this, current_coro_handle_.address());
    if (this != &other) {
      if (current_coro_handle_) {
        if (current_coro_handle_.done() || !current_coro_handle_.promise().started_) {
          current_coro_handle_.destroy();
        } else {
          current_coro_handle_.promise().awaitable_ = nullptr;
        }
      }
      current_coro_handle_ = other.current_coro_handle_;
      if (current_coro_handle_) {
        current_coro_handle_.promise().awaitable_ = this;
      }
      other.current_coro_handle_ = nullptr;
    }
    return *this;
  }

  /// co_await
  [[nodiscard]] bool await_ready() const noexcept {
    CORO_DEBUG_LIFECYCLE("awaitable: await_ready: %p, h: %p", this, current_coro_handle_.address());
    return current_coro_handle_.done();
  }

  template <typename Promise>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) {
    CORO_DEBUG_LIFECYCLE("awaitable: await_suspend: %p, h: %p, p.h: %p", this, current_coro_handle_.address(), h.address());
    // use bind executor or inherit from caller
    if (!current_coro_handle_.promise().executor_) {
      current_coro_handle_.promise().executor_ = h.promise().executor_;
    }
#ifdef CORO_ENABLE_LOCAL_STORAGE
    // Retain inherited storage without allocating an empty map for a read-only child.
    if (h.promise().local_storage_) {
      current_coro_handle_.promise().local_storage_ = h.promise().local_storage_;
      current_coro_handle_.promise().owns_local_storage_ = false;
    }
#endif
#ifdef CORO_DEBUG_ASYNC_STACK
    if constexpr (requires { h.promise().debug_node_.id_; }) {
      current_coro_handle_.promise().debug_node_.set_parent(h.promise().debug_node_.id_);
    } else {
      current_coro_handle_.promise().debug_node_.set_parent(0, h.address());
    }
#endif
    current_coro_handle_.promise().parent_handle_ = h;
    current_coro_handle_.promise().parent_executor_ = h.promise().executor_;
    auto* child_exec = current_coro_handle_.promise().executor_;
    if (child_exec != h.promise().executor_) {
      // Distinct executors may share a thread; queue nested starts to keep the native stack bounded.
      detail::post_child(child_exec, current_coro_handle_, current_coro_handle_.promise().parent_handle_,
                         current_coro_handle_.promise().parent_executor_);
      return std::noop_coroutine();
    }
    return current_coro_handle_;
  }

  T await_resume() {
    CORO_DEBUG_LIFECYCLE("awaitable: await_resume: %p, h: %p", this, current_coro_handle_.address());
    return current_coro_handle_.promise().get_value();
  }

  // Precondition: bind before the task is started or submitted for execution.
  auto bind_executor(executor& exec) noexcept {
    assert(current_coro_handle_ && !current_coro_handle_.promise().started_ && "bind_executor may only be called before task start");
    current_coro_handle_.promise().executor_ = &exec;
    return std::move(*this);
  }

  [[nodiscard]] executor* get_executor() const noexcept {
    return current_coro_handle_.promise().executor_;
  }

  void detach(auto& executor) {
    auto* exec_to_use = current_coro_handle_.promise().executor_ ? current_coro_handle_.promise().executor_ : &executor;
    exec_to_use->dispatch([coro = std::make_shared<awaitable<T>>(std::move(*this)), exec_to_use]() mutable {
      CORO_DEBUG_LIFECYCLE("awaitable: resume begin: %p, h: %p", coro.get(), coro->current_coro_handle_.address());
      auto handle = std::exchange(coro->current_coro_handle_, {});
      handle.promise().awaitable_ = nullptr;
      handle.promise().executor_ = exec_to_use;
      if (handle.done()) {
        handle.destroy();
      } else {
        handle.resume();
      }
      CORO_DEBUG_LIFECYCLE("awaitable: resume end: %p, h: %p", coro.get(), handle.address());
    });
  }

#ifndef CORO_DISABLE_EXCEPTION
  template <typename Function>
  auto with_callback(Function completion_handler, std::function<void(std::exception_ptr)> exception_handler = nullptr) {
    auto coro = [](awaitable<T> lazy, auto completion_handler, [[maybe_unused]] auto exception_handler) mutable -> awaitable<void> {
      try {
        if constexpr (std::is_void_v<T>) {
          co_await awaitable<T>{std::move(lazy)};
          completion_handler();
        } else {
          auto result = co_await awaitable<T>{std::move(lazy)};
          completion_handler(std::move(result));
        }
      } catch (...) {
        if (exception_handler) exception_handler(std::current_exception());
      }
    }(std::move(*this), std::move(completion_handler), std::move(exception_handler));
    return coro;
  }

  template <typename Function>
  void detach(auto& executor, Function completion_handler, std::function<void(std::exception_ptr)> exception_handler = nullptr) {
    auto coro = with_callback(std::move(completion_handler), std::move(exception_handler));
    coro.detach(executor);
  }
#else
  template <typename Function>
  auto with_callback(Function completion_handler) {
    auto coro = [](awaitable<T> lazy, auto completion_handler) mutable -> awaitable<void> {
      if constexpr (std::is_void_v<T>) {
        co_await awaitable<T>{std::move(lazy)};
        completion_handler();
      } else {
        auto result = co_await awaitable<T>{std::move(lazy)};
        completion_handler(std::move(result));
      }
    }(std::move(*this), std::move(completion_handler));
    return coro;
  }

  template <typename Function>
  void detach(auto& executor, Function completion_handler) {
    auto coro = with_callback(std::move(completion_handler));
    coro.detach(executor);
  }
#endif

  std::coroutine_handle<promise_type> current_coro_handle_;
};

template <typename T>
#ifdef CORO_DEBUG_ASYNC_STACK
awaitable<T> awaitable_promise<T>::get_return_object(detail::async_debug_location location) noexcept {
#else
awaitable<T> awaitable_promise<T>::get_return_object() noexcept {
#endif
  auto handle = std::coroutine_handle<awaitable_promise<T>>::from_promise(*this);
#ifdef CORO_DEBUG_ASYNC_STACK
  debug_node_.prepare_frame(handle.address(), location);
#endif
  CORO_DEBUG_LIFECYCLE("promise: get_return_object: p: %p, h: %p", this, handle.address());
  return awaitable<T>{handle};
}

/// callback_awaiter
namespace detail {

template <typename T>
struct callback_awaiter_base {
  using callback_function_no_executor = std::function<void(std::function<void(T)>)>;
  using callback_function_with_executor = std::function<void(executor*, std::function<void(T)>)>;

  T await_resume()
#ifdef CORO_DISABLE_EXCEPTION
      noexcept(std::is_nothrow_move_constructible_v<T>)
#endif
  {
#ifndef CORO_DISABLE_EXCEPTION
    if (exception_) {
      std::rethrow_exception(exception_);
    }
#endif
    assert(result_.has_value());
    return std::move(*result_);
  }

  template <typename U>
  void set_result(U&& value) {
#ifndef CORO_DISABLE_EXCEPTION
    exception_ = nullptr;
    try {
#endif
      result_.emplace(std::forward<U>(value));
#ifndef CORO_DISABLE_EXCEPTION
    } catch (...) {
      exception_ = std::current_exception();
    }
#endif
  }

  std::optional<T> result_;
#ifndef CORO_DISABLE_EXCEPTION
  std::exception_ptr exception_;
#endif
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
  using callback_function_no_executor = typename detail::callback_awaiter_base<T>::callback_function_no_executor;
  using callback_function_with_executor = typename detail::callback_awaiter_base<T>::callback_function_with_executor;
  std::variant<callback_function_no_executor, callback_function_with_executor> callback_function_;

  explicit callback_awaiter(callback_function_no_executor callback) noexcept : callback_function_(std::move(callback)) {}
  explicit callback_awaiter(callback_function_with_executor callback) noexcept : callback_function_(std::move(callback)) {}
  callback_awaiter(callback_awaiter&& other) noexcept(std::is_nothrow_move_constructible_v<detail::callback_awaiter_base<T>>)
      : detail::callback_awaiter_base<T>(std::move(other)), callback_function_(std::move(other.callback_function_)) {}

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  template <typename Promise>
  bool await_suspend(std::coroutine_handle<Promise> handle) {
    auto executor = handle.promise().executor_;
    assert(executor && "executor must be set before using callback_awaiter");
    phase_.store(phase::registering, std::memory_order_relaxed);
    switch (callback_function_.index()) {
      case 0: {
        auto& func = std::get<0>(callback_function_);
        if constexpr (std::is_void_v<T>) {
          func([handle, this]() {
            complete(handle);
          });
        } else {
          func([handle, this](T&& value) {
            this->set_result(std::move(value));
            complete(handle);
          });
        }
      } break;
      case 1: {
        auto& func = std::get<1>(callback_function_);
        if constexpr (std::is_void_v<T>) {
          func(executor, [handle, this]() {
            complete(handle);
          });
        } else {
          func(executor, [handle, this](T&& value) {
            this->set_result(std::move(value));
            complete(handle);
          });
        }
      } break;
    }
    if (phase_.load(std::memory_order_acquire) == phase::completed) {
      return false;
    }
    auto expected = phase::registering;
    return phase_.compare_exchange_strong(expected, phase::suspended, std::memory_order_acq_rel);
  }

 private:
  enum class phase { registering, suspended, completed };
  std::atomic<phase> phase_{phase::registering};

  template <typename Promise>
  void complete(std::coroutine_handle<Promise> handle) {
    // Exactly one side owns resumption: await_suspend for inline completion,
    // or the callback once suspension has been committed.
    if (phase_.exchange(phase::completed, std::memory_order_acq_rel) == phase::suspended) {
      // The task's executor is fixed before start; avoid capturing another pointer in the callback.
      detail::resume_via(handle.promise().executor_, handle);
    }
  }
};

/// current_executor_awaiter
namespace detail {

struct current_executor_awaiter {
  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  template <typename Promise>
  bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
    exec_ = h.promise().executor_;
    return false;
  }

  [[nodiscard]] executor* await_resume() const noexcept {
    return exec_;
  }

 private:
  executor* exec_ = nullptr;
};

}  // namespace detail

[[nodiscard]] inline detail::current_executor_awaiter current_executor() noexcept {
  return detail::current_executor_awaiter{};
}

template <typename T>
using async = awaitable<T>;

template <typename T>
void spawn(executor& executor, T&& coro) {
  coro.detach(executor);
}

namespace detail {

template <typename T>
async<void> spawn_local_impl(T awaitable) {
  auto* exec = co_await current_executor();
  if (exec) {
    spawn(*exec, std::move(awaitable));
  } else {
    co_await std::move(awaitable);
  }
}

}  // namespace detail

template <typename T>
[[nodiscard]] async<void> spawn_local(T&& awaitable) {
  // Transfer ownership before the lazy coroutine reaches initial_suspend.
  return detail::spawn_local_impl(std::move(awaitable));
}

}  // namespace coro
