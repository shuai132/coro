#pragma once

#include <atomic>
#include <coroutine>
#include <mutex>

#include "coro/coro.hpp"
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
template <typename MUTEX = std::mutex>
struct condition_variable {
 private:
  // Intrusive list node for waiting coroutines
  struct waiter_node {
    std::coroutine_handle<> handle;
    executor* exec{};
    waiter_node* next{};
  };

 public:
  condition_variable() : head_(nullptr), tail_(nullptr) {}

  // Wait releases the mutex and suspends the coroutine
  // When resumed by notify, the mutex is NOT automatically re-acquired
  // You must manually re-acquire the lock after wait returns
  // Similar to Go's cond.Wait()
  struct wait_awaitable {
    condition_variable* cv_;
    void* mtx_{};                 // Type-erased mutex pointer
    void (*unlock_fn_)(void*){};  // Function pointer to unlock
    waiter_node node_{};

    bool await_ready() noexcept {
      // Never ready - always suspend to release lock
      return false;
    }

    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h) {
      // Initialize the node
      node_.handle = h;
      node_.exec = h.promise().executor_;
      node_.next = nullptr;

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

      // Release the user's mutex before suspending (type-erased call)
      unlock_fn_(mtx_);
    }

    void await_resume() noexcept {
      // Mutex has been released, not re-acquired
      // User must manually re-lock
    }
  };

  // Wait for notification
  // Usage:
  //   co_await cv.wait(mtx);
  //   co_await mtx.lock();  // Must re-lock manually
  // Works with any mutex type (mutex, mutex_mt, mutex_st)
  template <typename MutexType>
  auto wait(MutexType& mtx) {
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
    if (node) {
      if (exec_to_use) {
        exec_to_use->dispatch([handle_to_resume]() {
          handle_to_resume.resume();
        });
      } else {
        handle_to_resume.resume();
      }
    }
  }

  // Notify all waiting coroutines
  // Similar to Go's cond.Broadcast()
  void notify_all() {
    waiter_node* nodes_to_resume = nullptr;

    {
      std::lock_guard<MUTEX> lock(mutex_);

      // Take the entire list
      nodes_to_resume = head_;
      head_ = nullptr;
      tail_ = nullptr;
    }

    // Resume all waiting coroutines outside the lock
    waiter_node* node = nodes_to_resume;
    while (node) {
      auto next_handle = node->handle;
      auto* next_exec = node->exec;
      waiter_node* next_node = node->next;

      if (next_exec) {
        next_exec->dispatch([next_handle]() {
          next_handle.resume();
        });
      } else {
        next_handle.resume();
      }

      node = next_node;
    }
  }

 private:
  mutable MUTEX mutex_;  // Internal mutex to protect the queue
  waiter_node* head_;    // Head of waiting list
  waiter_node* tail_;    // Tail of waiting list
};

// Type aliases for convenience
using condition_variable_mt = condition_variable<std::mutex>;
using condition_variable_st = condition_variable<dummy_mutex>;

}  // namespace coro
