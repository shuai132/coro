#pragma once

#include <atomic>
#include <coroutine>
#include <mutex>

#include "coro/coro.hpp"
#include "coro/dummy_mutex.hpp"

namespace coro {

// Coroutine mutex with configurable thread safety
// Template parameter MUTEX controls internal thread safety:
//   - std::mutex: Thread-safe for multithreaded use (default)
//   - dummy_mutex: No lock overhead for single-threaded use
template <typename MUTEX = std::mutex>
struct mutex_t {
 private:
  // Intrusive list node stored in the awaitable (which is part of coroutine frame)
  struct waiter_node {
    std::coroutine_handle<> handle;
    executor* exec{};
    waiter_node* next{};
  };

 public:
  mutex_t() : locked_(false), head_(nullptr), tail_(nullptr) {}

  bool is_locked() const {
    return locked_.load(std::memory_order_acquire);
  }

  struct lock_awaitable {
    mutex_t* m_;
    waiter_node node_{};  // Node lives as long as the awaitable (part of coroutine frame)

    bool await_ready() noexcept {
      // Try to acquire the lock atomically
      bool expected = false;
      if (m_->locked_.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
        return true;  // Don't suspend, we have the lock
      }
      return false;  // Suspend, as the lock is already taken
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      // Initialize the node
      node_.handle = h;
      node_.exec = h.promise().executor_;
      node_.next = nullptr;

      // Add to the waiting list (protected by internal mutex for thread safety)
      {
        std::lock_guard<MUTEX> lock(m_->list_lock_);
        waiter_node* old_tail = m_->tail_;
        if (old_tail) {
          old_tail->next = &node_;
        } else {
          m_->head_ = &node_;
        }
        m_->tail_ = &node_;
      }

      // Return true to suspend
      return true;
    }

    void await_resume() noexcept {
      // Lock acquired, nothing to return
    }
  };

  // Low-level lock operation, must be paired with unlock()
  // Usage: co_await mutex.lock();
  auto lock() {
    return lock_awaitable{this};
  }

  struct scoped_lock_awaitable {
    mutex_t* m_;
    waiter_node node_{};  // Node lives as long as the awaitable (part of coroutine frame)

    bool await_ready() noexcept {
      // Try to acquire the lock atomically
      bool expected = false;
      if (m_->locked_.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
        return true;  // Successfully acquired the lock, don't suspend
      }
      return false;  // Lock is held, need to suspend
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      // Initialize the node
      node_.handle = h;
      node_.exec = h.promise().executor_;
      node_.next = nullptr;

      // Add to the waiting list (protected by internal mutex for thread safety)
      {
        std::lock_guard<MUTEX> lock(m_->list_lock_);
        waiter_node* old_tail = m_->tail_;
        if (old_tail) {
          old_tail->next = &node_;
        } else {
          m_->head_ = &node_;
        }
        m_->tail_ = &node_;
      }

      // Return true to suspend
      return true;
    }

    auto await_resume() {
      // Return a RAII-style guard that releases the lock when destroyed
      return lock_guard{m_};
    }
  };

  // RAII-style scoped lock that returns a lock_guard
  // Usage: auto guard = co_await mutex.scoped_lock();
  auto scoped_lock() {
    return scoped_lock_awaitable{this};
  }

  void unlock() {
    // Check if there are waiting coroutines (protected by internal mutex)
    waiter_node* node;
    std::coroutine_handle<> next_handle;
    executor* next_exec;

    {
      std::lock_guard<MUTEX> lock(list_lock_);
      node = head_;
      if (node) {
        next_handle = node->handle;
        next_exec = node->exec;

        // Move head to next BEFORE resuming
        // This is critical: we must update the list before resume
        // because the node will be destroyed when the coroutine runs
        waiter_node* next_node = node->next;
        head_ = next_node;
        if (!next_node) {
          tail_ = nullptr;
        }
      }
    }

    if (node) {
      // The lock remains held (locked_ stays true), transfer to next coroutine
      // When next_handle resumes, it will return from co_await and the node
      // will be destroyed, but we've already updated head_ so it's safe
      if (next_exec) {
        next_exec->dispatch([next_handle]() {
          next_handle.resume();
        });
      } else {
        next_handle.resume();
      }
    } else {
      // No waiters, release the lock
      locked_.store(false, std::memory_order_release);
    }
  }

 private:
  std::atomic<bool> locked_;
  mutable MUTEX list_lock_;  // Protects head_ and tail_ for thread safety
  waiter_node* head_;        // Head of waiting list
  waiter_node* tail_;        // Tail of waiting list

 public:
  // RAII-style lock guard that unlocks when destroyed
  class lock_guard {
   public:
    explicit lock_guard(mutex_t* m) : mutex_(m) {}

    ~lock_guard() {
      unlock();
    }

    // Manually unlock the mutex
    void unlock() {
      mutex_t* m = mutex_.load(std::memory_order_acquire);
      if (m) {
        m->unlock();
        mutex_.store(nullptr, std::memory_order_release);
      }
    }

    // move semantics - transfer ownership
    lock_guard(lock_guard&& other) noexcept : mutex_(other.mutex_.load(std::memory_order_acquire)) {
      other.mutex_.store(nullptr, std::memory_order_release);
    }

    // disable copy
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

   private:
    std::atomic<mutex_t*> mutex_;
  };
};

// Type aliases for convenience
using mutex = mutex_t<std::mutex>;
using mutex_mt = mutex_t<std::mutex>;
using mutex_st = mutex_t<dummy_mutex>;

}  // namespace coro
