#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "coro/coro.hpp"
#include "coro/detail/resume.hpp"

namespace coro {

// ============================================================================
// when_all implementation
// ============================================================================

namespace detail {

// Helper to extract awaitable result type
template <typename T>
struct awaitable_traits;

template <typename T>
struct awaitable_traits<awaitable<T>> {
  using type = T;
};

template <typename T>
using awaitable_result_t = typename awaitable_traits<T>::type;

// Helper to convert void to a placeholder type
struct void_placeholder {};

template <typename T>
struct void_to_placeholder {
  using type = T;
};

template <>
struct void_to_placeholder<void> {
  using type = void_placeholder;
};

template <typename T>
using void_to_placeholder_t = typename void_to_placeholder<T>::type;

template <typename T>
struct when_all_storage {
  using type = std::optional<T>;
};

template <>
struct when_all_storage<void> {
  using type = void_placeholder;
};

template <typename T>
using when_all_storage_t = typename when_all_storage<T>::type;

// Storage for when_all results
template <typename... Ts>
struct when_all_state {
  std::tuple<when_all_storage_t<Ts>...> results;
  std::atomic<size_t> completed_count{0};
  size_t total_count = sizeof...(Ts);
  std::coroutine_handle<> parent_handle;
  executor* parent_exec = nullptr;
#ifdef CORO_DEBUG_ASYNC_STACK
  std::uint64_t debug_parent_id_ = 0;
#endif
  mutable std::mutex mtx;  // Mutex to protect shared state access
#ifndef CORO_DISABLE_EXCEPTION
  std::atomic<bool> exception_set{false};
  std::exception_ptr exception;  // Add exception support
#endif

  // Release stored values before returning or propagating an extraction error to the caller.
  struct result_cleanup {
    when_all_state* state_;

    ~result_cleanup() {
      std::apply(
          [](auto&... values) {
            (clear(values), ...);
          },
          state_->results);
    }

    template <typename T>
    static void clear(std::optional<T>& value) noexcept {
      value.reset();
    }
    static void clear(void_placeholder&) noexcept {}
  };

#ifndef CORO_DISABLE_EXCEPTION
  void set_exception(std::exception_ptr ex) {
    bool expected = false;
    {
      std::lock_guard<std::mutex> lock(mtx);
      if (exception_set.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        exception = std::move(ex);
      }
    }
    // A discarded exception's destructor may complete a sibling inline.
    ex = nullptr;
    increment_completed();
  }
#endif

  template <size_t Index, typename T>
  void set_result(std::optional<T>& value) {
    // Each child owns one result slot. User value operations may complete a sibling inline.
    std::get<Index>(results).emplace(std::move(*value));
    // A moved-from result can still have a destructor that refers to the parent's data.
    value.reset();
    increment_completed();
  }

  void increment_completed() {
    std::unique_lock<std::mutex> lock(mtx);
    size_t count = completed_count.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (count == total_count) {
      if (parent_handle) {
        lock.unlock();
        detail::post_resume_via(parent_exec, parent_handle);
      }
    }
  }
};

// Individual task wrapper for when_all
template <size_t Index, typename T, typename State>
awaitable<void> when_all_task(awaitable<T> task, std::shared_ptr<State> state) {
#ifndef CORO_DISABLE_EXCEPTION
  std::exception_ptr failure;
  try {
#endif
    if constexpr (std::is_void_v<T>) {
      // Consume a temporary so the child's frame is released before notifying the parent.
      co_await awaitable<T>{std::move(task)};
      state->increment_completed();
    } else {
      std::optional<T> result{std::in_place, co_await awaitable<T>{std::move(task)}};
      state->template set_result<Index>(result);
    }
#ifndef CORO_DISABLE_EXCEPTION
  } catch (...) {
    failure = std::current_exception();
  }
  // End the catch before publishing, so it cannot retain the exception after the parent returns.
  if (failure) {
    state->set_exception(std::exchange(failure, {}));
  }
#endif
}

// Awaiter for getting executor
template <typename State>
struct when_all_init_state_awaiter {
  std::shared_ptr<State> state_;

  bool await_ready() const noexcept {
    return false;
  }

  template <typename Promise>
  bool await_suspend(std::coroutine_handle<Promise> h) {
    {
      std::lock_guard<std::mutex> lock(state_->mtx);
      state_->parent_exec = h.promise().executor_;
#ifdef CORO_DEBUG_ASYNC_STACK
      state_->debug_parent_id_ = h.promise().debug_node_.id_;
#endif
    }
    if (state_->parent_exec && state_->total_count != 0) {
      // Queue the launch so nested combiners cannot recursively dispatch their children.
      detail::post_resume_via(state_->parent_exec, h);
      return true;
    }
    return false;
  }

  void await_resume() noexcept {}
};

// Forward declaration
template <typename State, typename ResultTuple, typename = void>
struct when_all_awaiter;

// Awaiter for waiting all tasks to complete (empty tuple case)
template <typename State>
struct when_all_awaiter<State, std::tuple<>> {
  std::shared_ptr<State> state_;

  bool await_ready() const noexcept {
    return state_->completed_count >= state_->total_count;
  }

  bool await_suspend(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->parent_handle = h;
    return state_->completed_count < state_->total_count;
  }

  std::tuple<> await_resume() {
    typename State::result_cleanup cleanup{state_.get()};
#ifndef CORO_DISABLE_EXCEPTION
    if (auto error = std::exchange(state_->exception, {})) {
      std::rethrow_exception(error);
    }
#endif
    return std::tuple<>{};
  }
};

// Helper to extract non-void values from storage tuple
template <typename ResultTuple, typename StorageTuple>
struct result_extractor {
  static ResultTuple extract(StorageTuple& storage) {
    return extract_impl(storage, std::make_index_sequence<std::tuple_size_v<ResultTuple>>{});
  }

 private:
  template <size_t... ResultIs>
  static ResultTuple extract_impl(StorageTuple& storage, std::index_sequence<ResultIs...>) {
    return ResultTuple{get_nth_non_void<ResultIs>(storage)...};
  }

  template <size_t N>
  static auto get_nth_non_void(StorageTuple& storage) {
    return get_nth_non_void_impl<N, 0>(storage, std::make_index_sequence<std::tuple_size_v<StorageTuple>>{});
  }

  template <size_t TargetIndex, size_t CurrentNonVoidCount, size_t... Is>
  static auto get_nth_non_void_impl(StorageTuple& storage, std::index_sequence<Is...>) {
    return get_nth_non_void_recursive<TargetIndex, 0, 0>(storage);
  }

  template <size_t TargetIndex, size_t CurrentIndex, size_t CurrentNonVoidCount>
  static auto get_nth_non_void_recursive(StorageTuple& storage) {
    if constexpr (CurrentIndex >= std::tuple_size_v<StorageTuple>) {
      // Should never reach here
      return std::tuple_element_t<TargetIndex, ResultTuple>{};
    } else {
      using current_type = std::tuple_element_t<CurrentIndex, StorageTuple>;
      if constexpr (std::is_same_v<current_type, void_placeholder>) {
        // Skip void placeholder
        return get_nth_non_void_recursive<TargetIndex, CurrentIndex + 1, CurrentNonVoidCount>(storage);
      } else {
        // Found a non-void value
        if constexpr (CurrentNonVoidCount == TargetIndex) {
          assert(std::get<CurrentIndex>(storage).has_value());
          return std::move(*std::get<CurrentIndex>(storage));
        } else {
          return get_nth_non_void_recursive<TargetIndex, CurrentIndex + 1, CurrentNonVoidCount + 1>(storage);
        }
      }
    }
  }
};

// Awaiter for waiting all tasks to complete (non-empty tuple)
template <typename State, typename... ResultTypes>
struct when_all_awaiter<State, std::tuple<ResultTypes...>> {
  std::shared_ptr<State> state_;

  bool await_ready() const noexcept {
    return state_->completed_count >= state_->total_count;
  }

  bool await_suspend(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->parent_handle = h;
    return state_->completed_count < state_->total_count;
  }

  std::tuple<ResultTypes...> await_resume() {
    typename State::result_cleanup cleanup{state_.get()};
#ifndef CORO_DISABLE_EXCEPTION
    if (auto error = std::exchange(state_->exception, {})) {
      std::rethrow_exception(error);
    }
#endif
    using storage_tuple = decltype(state_->results);
    return result_extractor<std::tuple<ResultTypes...>, storage_tuple>::extract(state_->results);
  }
};

// Awaiter for waiting all tasks to complete (one non-void child result)
template <typename State, typename ResultType>
struct when_all_single_awaiter {
  std::shared_ptr<State> state_;

  bool await_ready() const noexcept {
    return state_->completed_count >= state_->total_count;
  }

  bool await_suspend(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->parent_handle = h;
    return state_->completed_count < state_->total_count;
  }

  ResultType await_resume() {
    typename State::result_cleanup cleanup{state_.get()};
#ifndef CORO_DISABLE_EXCEPTION
    if (auto error = std::exchange(state_->exception, {})) {
      std::rethrow_exception(error);
    }
#endif
    // For single value, we need to extract the first (and only) non-void value
    return get_first_non_void<0>(state_->results);
  }

 private:
  template <size_t Index, typename StorageTuple>
  static ResultType get_first_non_void_impl(StorageTuple& storage) {
    if constexpr (Index >= std::tuple_size_v<StorageTuple>) {
      // Should never happen
      return ResultType{};
    } else {
      using current_type = std::tuple_element_t<Index, StorageTuple>;
      if constexpr (std::is_same_v<current_type, void_placeholder>) {
        return get_first_non_void_impl<Index + 1>(storage);
      } else {
        assert(std::get<Index>(storage).has_value());
        return std::move(*std::get<Index>(storage));
      }
    }
  }

  template <size_t Index>
  static ResultType get_first_non_void(decltype(state_->results)& storage) {
    return get_first_non_void_impl<Index>(storage);
  }
};

// Check if all types are void
template <typename... Ts>
struct all_void : std::false_type {};

template <>
struct all_void<> : std::true_type {};

template <typename... Ts>
struct all_void<void, Ts...> : all_void<Ts...> {};

template <typename T, typename... Ts>
struct all_void<T, Ts...> : std::false_type {};

// Filter out void types from a type list
template <typename... Ts>
struct filter_void;

template <>
struct filter_void<> {
  using type = std::tuple<>;
};

template <typename... Rest>
struct filter_void<void, Rest...> {
  using type = typename filter_void<Rest...>::type;
};

template <typename T, typename... Rest>
struct filter_void<T, Rest...> {
  using type = decltype(std::tuple_cat(std::declval<std::tuple<T>>(), std::declval<typename filter_void<Rest...>::type>()));
};

template <typename... Ts>
using filter_void_t = typename filter_void<Ts...>::type;

// Helper to get result type based on filtered tuple size
template <typename Filtered, size_t Size>
struct when_all_result_type_from_filtered;

template <typename Filtered>
struct when_all_result_type_from_filtered<Filtered, 0> {
  using type = std::tuple<>;
};

template <typename Filtered>
struct when_all_result_type_from_filtered<Filtered, 1> {
  using type = std::tuple_element_t<0, Filtered>;
};

template <typename Filtered, size_t Size>
struct when_all_result_type_from_filtered {
  using type = Filtered;
};

// Helper to determine result type
template <typename... Ts>
struct when_all_result_type {
  using filtered = filter_void_t<Ts...>;
  using type = typename when_all_result_type_from_filtered<filtered, std::tuple_size_v<filtered>>::type;
};

template <>
struct when_all_result_type<> {
  using type = std::tuple<>;
};

template <typename... Ts>
using when_all_result_type_t = typename when_all_result_type<Ts...>::type;

// Void awaiter for when all tasks return void
template <typename State>
struct when_all_void_awaiter {
  std::shared_ptr<State> state_;

  bool await_ready() const noexcept {
    return state_->completed_count >= state_->total_count;
  }

  bool await_suspend(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->parent_handle = h;
    return state_->completed_count < state_->total_count;
  }

  void await_resume() {
#ifndef CORO_DISABLE_EXCEPTION
    if (auto error = std::exchange(state_->exception, {})) {
      std::rethrow_exception(error);
    }
#endif
  }
};

// Helper for expanding parameter pack
template <typename... Awaitables, size_t... Is>
awaitable<when_all_result_type_t<awaitable_result_t<Awaitables>...>> when_all_impl(std::index_sequence<Is...>, Awaitables... awaitables) {
  using result_tuple = when_all_result_type_t<awaitable_result_t<Awaitables>...>;
  using state_type = when_all_state<awaitable_result_t<Awaitables>...>;
  auto state = std::make_shared<state_type>();

  // Init state from current coroutine context
  co_await when_all_init_state_awaiter<state_type>{state};

  // Launch all tasks
#ifndef CORO_DISABLE_EXCEPTION
  size_t launched_count = 0;
  std::exception_ptr launch_failure;
#endif
  [[maybe_unused]] auto launch_task = [&]<size_t I, typename Aw>(executor* parent_exec, Aw& aw) {
    auto* exec = aw.get_executor() ? aw.get_executor() : parent_exec;
    assert(exec && "when_all requires an executor unless every child awaitable is already bound to one");
    auto task = when_all_task<I>(std::move(aw), state);
#ifdef CORO_DEBUG_ASYNC_STACK
    task.current_coro_handle_.promise().debug_node_.set_parent(state->debug_parent_id_);
#endif
    task.bind_executor(*exec).detach(*exec);
#ifndef CORO_DISABLE_EXCEPTION
    ++launched_count;
#endif
  };
#ifndef CORO_DISABLE_EXCEPTION
  try {
#endif
    (launch_task.template operator()<Is>(state->parent_exec, awaitables), ...);
#ifndef CORO_DISABLE_EXCEPTION
  } catch (...) {
    launch_failure = std::current_exception();
  }
  if (launch_failure) {
    {
      std::lock_guard<std::mutex> lock(state->mtx);
      // Count the failed launch as an error, then wait for the children already started.
      state->total_count = launched_count + 1;
    }
    state->set_exception(std::exchange(launch_failure, {}));
  }
#endif

  // Wait for all to complete
  if constexpr (std::tuple_size_v<filter_void_t<awaitable_result_t<Awaitables>...>> == 1) {
    co_return co_await when_all_single_awaiter<state_type, result_tuple>{state};
  } else {
    co_return co_await when_all_awaiter<state_type, result_tuple>{state};
  }
}

}  // namespace detail

// when_all: waits for all awaitables to complete and returns tuple of results
template <typename... Awaitables>
auto when_all(Awaitables&&... awaitables) {
  return detail::when_all_impl<std::decay_t<Awaitables>...>(std::index_sequence_for<std::decay_t<Awaitables>...>{},
                                                            std::forward<Awaitables>(awaitables)...);
}

// ============================================================================
// when_any implementation
// ============================================================================

namespace detail {

// State for when_any (base case with actual values)
template <bool AllVoid, typename... Ts>
struct when_any_state_impl {
  std::variant<std::monostate, void_to_placeholder_t<Ts>...> result;
  size_t completed_index = 0;
  std::atomic<bool> completed{false};
  bool claimed = false;
  std::coroutine_handle<> parent_handle;
  executor* parent_exec = nullptr;
#ifdef CORO_DEBUG_ASYNC_STACK
  std::uint64_t debug_parent_id_ = 0;
#endif
  mutable std::mutex mtx;  // Mutex to protect shared state access
#ifndef CORO_DISABLE_EXCEPTION
  std::exception_ptr exception;  // Add exception support
#endif

  // Other branches can keep this state alive after the winning result has been consumed.
  struct result_cleanup {
    when_any_state_impl* state_;

    ~result_cleanup() {
      state_->result.template emplace<0>();
    }
  };

  bool try_claim() {
    std::lock_guard<std::mutex> lock(mtx);
    if (claimed) {
      return false;
    }
    claimed = true;
    return true;
  }

  void publish_completion() {
    std::unique_lock<std::mutex> lock(mtx);
    // Keep winner selection separate from publication: user values may resume other branches.
    completed.store(true, std::memory_order_release);
    if (parent_handle) {
      lock.unlock();
      detail::post_resume_via(parent_exec, parent_handle);
    }
  }

#ifndef CORO_DISABLE_EXCEPTION
  void set_exception(std::exception_ptr ex) {
    if (try_claim()) {
      exception = std::move(ex);
      ex = nullptr;
      publish_completion();
    }
  }
#endif

  template <size_t Index, typename T>
  void set_result(std::optional<T>& value) {
    if (!try_claim()) {
      return;
    }
    completed_index = Index;
#ifndef CORO_DISABLE_EXCEPTION
    try {
#endif
      result.template emplace<Index + 1>(std::move(*value));
#ifndef CORO_DISABLE_EXCEPTION
    } catch (...) {
      exception = std::current_exception();
    }
#endif
    // Moving or destroying a user value must not hold the shared-state mutex.
    // End the catch and release the source before publishing completion to the parent.
    value.reset();
    publish_completion();
  }

  template <size_t Index>
  void set_completed_at() {
    if (try_claim()) {
      completed_index = Index;
      result.template emplace<Index + 1>(void_placeholder{});
      publish_completion();
    }
  }
};

// State for when_any (all void case)
template <typename... Ts>
struct when_any_state_impl<true, Ts...> {
  size_t completed_index = 0;
  std::atomic<bool> completed{false};
  std::coroutine_handle<> parent_handle;
  executor* parent_exec = nullptr;
#ifdef CORO_DEBUG_ASYNC_STACK
  std::uint64_t debug_parent_id_ = 0;
#endif
  mutable std::mutex mtx;  // Mutex to protect shared state access
#ifndef CORO_DISABLE_EXCEPTION
  std::exception_ptr exception;  // Add exception support
#endif

#ifndef CORO_DISABLE_EXCEPTION
  void set_exception(std::exception_ptr ex) {
    std::unique_lock<std::mutex> lock(mtx);
    if (!completed.load(std::memory_order_relaxed)) {
      exception = std::move(ex);
      ex = nullptr;
      // Publish completion only after the result or exception is fully initialized.
      completed.store(true, std::memory_order_release);
      if (parent_handle) {
        lock.unlock();
        detail::post_resume_via(parent_exec, parent_handle);
      }
    }
  }
#endif

  template <size_t Index>
  void set_completed_at() {
    std::unique_lock<std::mutex> lock(mtx);
    if (!completed.load(std::memory_order_relaxed)) {
      completed_index = Index;
      // Publish completion only after the result or exception is fully initialized.
      completed.store(true, std::memory_order_release);
      if (parent_handle) {
        lock.unlock();
        detail::post_resume_via(parent_exec, parent_handle);
      }
    }
  }
};

template <typename... Ts>
struct when_any_state : when_any_state_impl<all_void<Ts...>::value, Ts...> {};

// Individual task wrapper for when_any
template <size_t Index, typename T, typename State>
awaitable<void> when_any_task(awaitable<T> task, std::shared_ptr<State> state) {
#ifndef CORO_DISABLE_EXCEPTION
  std::exception_ptr failure;
  try {
#endif
    if constexpr (std::is_void_v<T>) {
      // Consume a temporary so the child's frame is released before notifying the parent.
      co_await awaitable<T>{std::move(task)};
      state->template set_completed_at<Index>();
    } else {
      std::optional<T> result{std::in_place, co_await awaitable<T>{std::move(task)}};
      state->template set_result<Index>(result);
    }
#ifndef CORO_DISABLE_EXCEPTION
  } catch (...) {
    failure = std::current_exception();
  }
  // End the catch before publishing, so it cannot retain the exception after the parent returns.
  if (failure) {
    state->set_exception(std::exchange(failure, {}));
  }
#endif
}

// Helper to select correct when_any_result implementation
template <bool AllVoid, typename... Ts>
struct when_any_result_impl_selector;

// Specialization for all void
template <typename... Ts>
struct when_any_result_impl_selector<true, Ts...> {
  struct type {
    size_t index;
  };
};

// Specialization for at least one non-void (uses void_placeholder for void types)
template <typename... Ts>
struct when_any_result_impl_selector<false, Ts...> {
  struct type {
    size_t index;
    std::variant<std::monostate, void_to_placeholder_t<Ts>...> value;

    template <size_t I>
    auto& get() {
      return std::get<I + 1>(value);
    }

    template <size_t I>
    const auto& get() const {
      return std::get<I + 1>(value);
    }
  };
};

// Result type for when_any
template <typename... Ts>
struct when_any_result : when_any_result_impl_selector<all_void<Ts...>::value, Ts...>::type {};

// Awaiter for getting executor in when_any
template <typename State>
struct when_any_init_state_awaiter {
  std::shared_ptr<State> state_;

  bool await_ready() const noexcept {
    return false;
  }

  template <typename Promise>
  bool await_suspend(std::coroutine_handle<Promise> h) {
    {
      std::lock_guard<std::mutex> lock(state_->mtx);
      state_->parent_exec = h.promise().executor_;
#ifdef CORO_DEBUG_ASYNC_STACK
      state_->debug_parent_id_ = h.promise().debug_node_.id_;
#endif
    }
    if (state_->parent_exec) {
      // Queue the launch so nested combiners cannot recursively dispatch their children.
      detail::post_resume_via(state_->parent_exec, h);
      return true;
    }
    return false;
  }

  void await_resume() noexcept {}
};

// Awaiter for waiting any task to complete (with values)
template <typename State, typename ResultType, bool AllVoid>
struct when_any_awaiter_impl {
  std::shared_ptr<State> state_;

  bool await_ready() const noexcept {
    return state_->completed;
  }

  bool await_suspend(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->parent_handle = h;
    return !state_->completed;
  }

  ResultType await_resume() {
    typename State::result_cleanup cleanup{state_.get()};
#ifndef CORO_DISABLE_EXCEPTION
    if (auto error = std::exchange(state_->exception, {})) {
      std::rethrow_exception(error);
    }
#endif
    return ResultType{state_->completed_index, std::move(state_->result)};
  }
};

// Awaiter for waiting any task to complete (all void)
template <typename State, typename ResultType>
struct when_any_awaiter_impl<State, ResultType, true> {
  std::shared_ptr<State> state_;

  bool await_ready() const noexcept {
    return state_->completed;
  }

  bool await_suspend(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(state_->mtx);
    state_->parent_handle = h;
    return !state_->completed;
  }

  ResultType await_resume() {
#ifndef CORO_DISABLE_EXCEPTION
    if (auto error = std::exchange(state_->exception, {})) {
      std::rethrow_exception(error);
    }
#endif
    return ResultType{state_->completed_index};
  }
};

template <typename State, typename ResultType, typename... Ts>
struct when_any_awaiter : when_any_awaiter_impl<State, ResultType, all_void<Ts...>::value> {
  using base = when_any_awaiter_impl<State, ResultType, all_void<Ts...>::value>;
  using base::state_;

  explicit when_any_awaiter(std::shared_ptr<State> state) : base{state} {}
};

// Helper for expanding parameter pack
template <typename... Awaitables, size_t... Is>
awaitable<when_any_result<awaitable_result_t<Awaitables>...>> when_any_impl(std::index_sequence<Is...>, Awaitables... awaitables) {
  using result_type = when_any_result<awaitable_result_t<Awaitables>...>;
  using state_type = when_any_state<awaitable_result_t<Awaitables>...>;
  auto state = std::make_shared<state_type>();

  // Init state from current coroutine context
  co_await when_any_init_state_awaiter<state_type>{state};

  // Launch all tasks
  auto launch_task = [&]<size_t I, typename Aw>(executor* parent_exec, Aw& aw) {
    auto* exec = aw.get_executor() ? aw.get_executor() : parent_exec;
    assert(exec && "when_any requires an executor unless every child awaitable is already bound to one");
    auto task = when_any_task<I>(std::move(aw), state);
#ifdef CORO_DEBUG_ASYNC_STACK
    task.current_coro_handle_.promise().debug_node_.set_parent(state->debug_parent_id_);
#endif
    task.bind_executor(*exec).detach(*exec);
  };
  (launch_task.template operator()<Is>(state->parent_exec, awaitables), ...);

  // Wait for first to complete
  co_return co_await when_any_awaiter<state_type, result_type, awaitable_result_t<Awaitables>...>{state};
}

}  // namespace detail

// when_any: returns as soon as any awaitable completes
template <typename... Awaitables>
auto when_any(Awaitables&&... awaitables) {
  static_assert(sizeof...(Awaitables) > 0, "when_any requires at least one awaitable");
  return detail::when_any_impl<std::decay_t<Awaitables>...>(std::index_sequence_for<std::decay_t<Awaitables>...>{},
                                                            std::forward<Awaitables>(awaitables)...);
}

}  // namespace coro
