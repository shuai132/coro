#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>
#include <exception>
#include <mutex>
#include <utility>

#include "coro/coro.hpp"
#include "coro/detail/resume.hpp"
#include "coro/detail/thread_support.hpp"
#include "coro/detail/waiter_registration.hpp"
#include "coro/dummy_mutex.hpp"  // For dummy_mutex
#include "coro/mutex.hpp"

namespace coro {

// Coroutine-safe condition variable, similar to Go's sync.Cond
// Must be used with coro::mutex
// Note: Unlike Go's cond.Wait(), after being notified, you need to manually re-acquire the lock
//
// Template parameter MUTEX controls internal thread safety:
//   - std::mutex: Thread-safe for multithreaded use (default)
//   - dummy_mutex: No lock overhead for single-threaded use
#if CORO_DETAIL_HAS_STD_MUTEX
template <typename MUTEX = std::mutex>
#else
template <typename MUTEX>
#endif
struct condition_variable_t {
 private:
  // Intrusive list node for waiting coroutines
  struct waiter_node {
    std::coroutine_handle<> handle;
    executor* exec{};
    waiter_node* next{};
    detail::waiter_registration<MUTEX> registration_;
  };

 public:
  condition_variable_t() : head_(nullptr), tail_(nullptr) {}

  ~condition_variable_t() {
    assert(head_ == nullptr);
    assert(tail_ == nullptr);
  }

  condition_variable_t(const condition_variable_t&) = delete;
  condition_variable_t(condition_variable_t&&) = delete;
  condition_variable_t& operator=(const condition_variable_t&) = delete;
  condition_variable_t& operator=(condition_variable_t&&) = delete;

  // Wait releases the mutex and suspends the coroutine
  // When resumed by notify, the mutex is NOT automatically re-acquired
  // You must manually re-acquire the lock after wait returns
  // Similar to Go's cond.Wait()
  struct wait_awaitable {
    condition_variable_t* cv_;
    void* mtx_{};                 // Type-erased mutex pointer
    void (*unlock_fn_)(void*){};  // Function pointer to unlock
    waiter_node node_{};
#ifndef CORO_DISABLE_EXCEPTION
    std::exception_ptr unlock_exception_{};
#endif

    bool await_ready() noexcept {
      // Never ready - always suspend to release lock
      return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      auto* mtx = mtx_;
      auto unlock_fn = unlock_fn_;

      // Initialize the node
      node_.handle = h;
      node_.exec = h.promise().executor_;
      node_.next = nullptr;
      node_.registration_.reset();

      {
        // Lock internal mutex to protect queue
        std::lock_guard<MUTEX> lock(cv_->mutex_);

        // Add to waiting list
        waiter_node* old_tail = cv_->tail_;
        if (old_tail) {
          old_tail->next = &node_;
        } else {
          cv_->head_ = &node_;
        }
        cv_->tail_ = &node_;
      }

      // Release the user's mutex before this waiter is allowed to resume.
#ifndef CORO_DISABLE_EXCEPTION
      try {
#endif
        unlock_fn(mtx);
#ifndef CORO_DISABLE_EXCEPTION
      } catch (...) {
        unlock_exception_ = std::current_exception();
        if (cv_->remove_waiter(&node_)) {
          return false;
        }
        // A notifier already owns the node. Finish the handoff before propagating,
        // so unwinding cannot destroy a frame that the notifier still accesses.
      }
#endif

      // Publish suspension only after all mutex operations return. A notification can now destroy this awaiter.
      return node_.registration_.suspend();
    }

    void await_resume()
#ifdef CORO_DISABLE_EXCEPTION
        noexcept
#endif
    {
#ifndef CORO_DISABLE_EXCEPTION
      if (auto error = std::exchange(unlock_exception_, {})) {
        std::rethrow_exception(error);
      }
#endif
      // Mutex has been released, not re-acquired
      // User must manually re-lock
    }
  };

  // Wait for notification
  // Usage:
  //   co_await cv.wait(mtx);
  //   co_await mtx.lock();  // Must re-lock manually
  // Works with coro::mutex and compatible types providing unlock().
  template <typename MutexType>
  auto wait(MutexType& mtx) noexcept {
    return wait_awaitable{this, static_cast<void*>(&mtx), [](void* m) {
                            static_cast<MutexType*>(m)->unlock();
                          }};
  }

  // Wait with predicate - waits until the predicate returns true
  // Similar to C++ std::condition_variable::wait(lock, pred)
  // Equivalent to: while (!pred()) { wait(mtx); lock(); }
  // Usage:
  //   co_await cv.wait(mtx, [&]{ return ready; });
  //   // Lock is automatically re-acquired when predicate is true
  template <typename MutexType, typename Predicate>
  async<void> wait(MutexType& mtx, Predicate pred) {
    while (!pred()) {
      co_await wait(mtx);
      co_await mtx.lock();
    }
    // Lock is held when predicate is true
  }

  // Notify one waiting coroutine (if any)
  // Similar to Go's cond.Signal()
  void notify_one() {
    waiter_node* node = nullptr;
    std::coroutine_handle<> handle_to_resume;
    executor* exec_to_use = nullptr;

    {
      std::lock_guard<MUTEX> lock(mutex_);

      node = head_;
      if (node) {
        // Move head to next node
        head_ = node->next;
        if (!head_) {
          tail_ = nullptr;
        }

        handle_to_resume = node->handle;
        exec_to_use = node->exec;
      }
    }

    // Resume the waiting coroutine outside the lock
    if (node && node->registration_.notify()) {
      detail::post_resume_via(exec_to_use, handle_to_resume);
    }
  }

  // Notify all waiting coroutines
  // Similar to Go's cond.Broadcast()
  void notify_all() {
    waiter_node* nodes = nullptr;

    {
      std::lock_guard<MUTEX> lock(mutex_);
      nodes = head_;
      head_ = nullptr;
      tail_ = nullptr;
    }

    while (nodes) {
      auto handle = nodes->handle;
      auto* exec = nodes->exec;
      auto* next = nodes->next;
      // Cache everything before notification publishes completion to a registering waiter.
      if (nodes->registration_.notify()) {
        detail::post_resume_via(exec, handle);
      }
      nodes = next;
    }
  }

 private:
#ifndef CORO_DISABLE_EXCEPTION
  bool remove_waiter(waiter_node* node) {
    std::lock_guard<MUTEX> lock(mutex_);
    waiter_node* previous = nullptr;
    for (auto* current = head_; current; current = current->next) {
      if (current == node) {
        if (previous) {
          previous->next = current->next;
        } else {
          head_ = current->next;
        }
        if (tail_ == current) {
          tail_ = previous;
        }
        current->next = nullptr;
        return true;
      }
      previous = current;
    }
    return false;
  }

#endif

  mutable MUTEX mutex_;  // Internal mutex to protect the queue
  waiter_node* head_;    // Head of waiting list
  waiter_node* tail_;    // Tail of waiting list
};

// Type aliases for convenience
#if CORO_DETAIL_HAS_STD_MUTEX
using condition_variable = condition_variable_t<std::mutex>;
using condition_variable_mt = condition_variable_t<std::mutex>;
#endif
using condition_variable_st = condition_variable_t<dummy_mutex>;

}  // namespace coro
