#pragma once

#include <cassert>
#include <coroutine>
#include <mutex>

#include "coro/coro.hpp"
#include "coro/detail/resume.hpp"
#include "coro/dummy_mutex.hpp"

namespace coro {

// Coroutine-safe event synchronization primitive
// The event allows one or more coroutines to wait for an event to be set before proceeding.
//
// When the event is set, all waiting coroutines are resumed on the thread that sets the event.
// If the event is already set when a coroutine waits, it will simply continue executing
// with no suspend or wait time incurred.
//
// Once set, the event stays set until explicitly cleared with clear().
//
// Template parameter MUTEX controls internal thread safety:
//   - std::mutex: Thread-safe for multithreaded use (default)
//   - dummy_mutex: No lock overhead for single-threaded use
template <typename MUTEX = std::mutex>
struct event_t {
 private:
  struct waiter_node {
    std::coroutine_handle<> handle;
    executor* exec{};
    waiter_node* next{};
  };

  struct wait_awaitable {
    event_t* event_;
    waiter_node node_{};

    bool await_ready() {
      std::lock_guard<MUTEX> lock(event_->mutex_);
      return event_->set_;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      node_.handle = h;
      node_.exec = h.promise().executor_;
      node_.next = nullptr;

      {
        std::lock_guard<MUTEX> lock(event_->mutex_);
        if (event_->set_) {
          return false;
        }

        if (event_->tail_) {
          event_->tail_->next = &node_;
        } else {
          event_->head_ = &node_;
        }
        event_->tail_ = &node_;
      }

      return true;
    }

    void await_resume() noexcept {}
  };

 public:
  // Construct an event in the unset state
  event_t() = default;

  explicit event_t(bool initially_set) : set_(initially_set) {}

  ~event_t() {
    assert(head_ == nullptr);
    assert(tail_ == nullptr);
  }

  event_t(const event_t&) = delete;
  event_t(event_t&&) = delete;
  event_t& operator=(const event_t&) = delete;
  event_t& operator=(event_t&&) = delete;

  // Set the event and wake up all waiting coroutines
  // All current and future waiters will proceed until clear() is called
  // Usage: evt.set();
  void set() {
    waiter_node* nodes_to_resume = nullptr;

    {
      std::lock_guard<MUTEX> lock(mutex_);
      if (set_) {
        return;
      }

      set_ = true;
      nodes_to_resume = head_;
      head_ = nullptr;
      tail_ = nullptr;
    }

    resume_all(nodes_to_resume);
  }

  // Clear the event
  // Future waits will block until set() is called again
  // Usage: evt.clear();
  void clear() {
    std::lock_guard<MUTEX> lock(mutex_);
    set_ = false;
  }

  // Wait for the event to be set
  // If already set, returns immediately without suspending
  // Usage: co_await evt.wait();
  auto wait() const {
    return wait_awaitable{const_cast<event_t*>(this)};
  }

  // Support direct co_await on event object
  // Usage: co_await evt;
  auto operator co_await() const noexcept {
    return wait_awaitable{const_cast<event_t*>(this)};
  }

  // Check if the event is set (non-blocking)
  // Returns true if the event is set
  bool is_set() const {
    std::lock_guard<MUTEX> lock(mutex_);
    return set_;
  }

 private:
  static void resume_all(waiter_node* node) {
    while (node) {
      auto next_handle = node->handle;
      auto* next_exec = node->exec;
      waiter_node* next_node = node->next;

      detail::resume_via(next_exec, next_handle);

      node = next_node;
    }
  }

  mutable MUTEX mutex_;
  bool set_{false};
  waiter_node* head_{nullptr};
  waiter_node* tail_{nullptr};
};

// Type aliases for convenience
using event = event_t<std::mutex>;
using event_mt = event_t<std::mutex>;
using event_st = event_t<dummy_mutex>;

}  // namespace coro
