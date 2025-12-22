#pragma once

#include <atomic>
#include <coroutine>
#include <mutex>

#include "coro/coro.hpp"
#include "coro/dummy_mutex.hpp"

namespace coro {

// Coroutine-safe wait group, similar to Go's sync.WaitGroup
// Allows coordination between multiple coroutines
//
// Template parameter MUTEX controls internal thread safety:
//   - std::mutex: Thread-safe for multithreaded use (default)
//   - dummy_mutex: No lock overhead for single-threaded use
template <typename MUTEX = std::mutex>
struct wait_group_t {
 private:
  // Intrusive list node for waiting coroutines
  struct waiter_node {
    std::coroutine_handle<> handle;
    executor* exec{};
    waiter_node* next{};
  };

 public:
  wait_group_t() : counter_(0), head_(nullptr), tail_(nullptr) {}

  // Add delta to the wait group counter
  // Similar to Go's wg.Add(delta)
  void add(int delta) {
    waiter_node* nodes_to_resume = nullptr;

    {
      std::lock_guard<MUTEX> lock(mutex_);
      counter_ += delta;

      // Check if counter reached zero and collect waiters
      if (counter_ == 0) {
        nodes_to_resume = head_;
        head_ = nullptr;
        tail_ = nullptr;
      }
    }

    // Resume all waiting coroutines outside the lock
    if (nodes_to_resume) {
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
  }

  // Decrement the wait group counter by 1
  // Similar to Go's wg.Done()
  void done() {
    add(-1);
  }

  // Wait until the wait group counter becomes zero
  // Similar to Go's wg.Wait()
  struct wait_awaitable {
    wait_group_t* wg_;
    waiter_node node_{};

    bool await_ready() noexcept {
      // Check if counter is already zero
      std::lock_guard<MUTEX> lock(wg_->mutex_);
      return wg_->counter_ == 0;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      // Initialize the node
      node_.handle = h;
      node_.exec = h.promise().executor_;
      node_.next = nullptr;

      {
        std::lock_guard<MUTEX> lock(wg_->mutex_);

        // Double check counter after acquiring lock
        if (wg_->counter_ == 0) {
          return false;  // Don't suspend
        }

        // Add to waiting list
        waiter_node* old_tail = wg_->tail_;
        if (old_tail) {
          old_tail->next = &node_;
        } else {
          wg_->head_ = &node_;
        }
        wg_->tail_ = &node_;
      }

      // Suspend
      return true;
    }

    void await_resume() noexcept {
      // Counter has reached zero
    }
  };

  // Wait for the counter to reach zero
  // Usage: co_await wg.wait();
  auto wait() {
    return wait_awaitable{this};
  }

  // Get current counter value (for debugging/testing)
  int get_count() const {
    std::lock_guard<MUTEX> lock(mutex_);
    return counter_;
  }

 private:
  int counter_;          // Current counter value
  mutable MUTEX mutex_;  // Internal mutex to protect counter and queue
  waiter_node* head_;    // Head of waiting list
  waiter_node* tail_;    // Tail of waiting list
};

// Type aliases for convenience
using wait_group = wait_group_t<std::mutex>;
using wait_group_mt = wait_group_t<std::mutex>;
using wait_group_st = wait_group_t<dummy_mutex>;

}  // namespace coro
