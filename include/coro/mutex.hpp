#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>

#include "coro/coro.hpp"

namespace coro {

// Lock-free coroutine mutex
// Inspired by async_simple's Mutex implementation
// Uses atomic operations to manage lock state and waiter queue without internal locks
struct mutex_t {
 private:
  // Intrusive list node stored in the awaitable (which is part of coroutine frame)
  struct waiter_node {
    std::coroutine_handle<> handle;
    executor* exec{};
    waiter_node* next{};
  };

 public:
  mutex_t() : state_(unlocked_state()), waiters_(nullptr) {}

  ~mutex_t() {
    // Check there are no waiters waiting to acquire the lock
    assert(state_.load(std::memory_order_relaxed) == unlocked_state() || state_.load(std::memory_order_relaxed) == nullptr);
    assert(waiters_ == nullptr);
  }

  mutex_t(const mutex_t&) = delete;
  mutex_t(mutex_t&&) = delete;
  mutex_t& operator=(const mutex_t&) = delete;
  mutex_t& operator=(mutex_t&&) = delete;

  // Check if the mutex is currently locked
  // Note: This is a snapshot and may be stale immediately after return
  bool is_locked() const noexcept {
    return state_.load(std::memory_order_relaxed) != const_cast<mutex_t*>(this);
  }

  // Try to lock the mutex synchronously
  // Returns true if lock was acquired, false otherwise
  bool try_lock() noexcept {
    void* old_value = unlocked_state();
    return state_.compare_exchange_strong(old_value, nullptr, std::memory_order_acquire, std::memory_order_relaxed);
  }

  struct lock_awaitable {
    mutex_t* m_;
    waiter_node node_{};  // Node lives as long as the awaitable (part of coroutine frame)

    bool await_ready() noexcept {
      return m_->try_lock();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      node_.handle = h;
      node_.exec = h.promise().executor_;
      return m_->lock_async_impl(&node_);
    }

    void await_resume() noexcept {}
  };

  struct scoped_lock_awaitable {
    mutex_t* m_;
    waiter_node node_{};  // Node lives as long as the awaitable (part of coroutine frame)

    bool await_ready() noexcept {
      return m_->try_lock();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      node_.handle = h;
      node_.exec = h.promise().executor_;
      return m_->lock_async_impl(&node_);
    }

    auto await_resume() noexcept {
      // Return a RAII-style guard that releases the lock when destroyed
      return lock_guard{m_};
    }
  };

  // Low-level lock operation, must be paired with unlock()
  // Usage: co_await mutex.lock();
  auto lock() noexcept {
    return lock_awaitable{this};
  }

  // RAII-style scoped lock that returns a lock_guard
  // Usage: auto guard = co_await mutex.scoped_lock();
  auto scoped_lock() noexcept {
    return scoped_lock_awaitable{this};
  }

  // Unlock the mutex
  // If there are waiting coroutines, the next one will be resumed
  void unlock() noexcept {
    assert(state_.load(std::memory_order_relaxed) != unlocked_state());

    auto* waiters_head = waiters_;
    if (waiters_head == nullptr) {
      void* current_state = state_.load(std::memory_order_relaxed);
      if (current_state == nullptr) {
        // Looks like there are no waiters waiting to acquire the lock
        // Try to unlock it - use compare-exchange to handle race with new waiters
        const bool released_lock =
            state_.compare_exchange_strong(current_state, unlocked_state(), std::memory_order_release, std::memory_order_relaxed);
        if (released_lock) {
          return;
        }
      }

      // There are some waiters that have been newly queued
      // Dequeue them and reverse their order from LIFO to FIFO
      current_state = state_.exchange(nullptr, std::memory_order_acquire);
      assert(current_state != unlocked_state());
      assert(current_state != nullptr);

      auto* waiter = static_cast<waiter_node*>(current_state);
      do {
        auto* temp = waiter->next;
        waiter->next = waiters_head;
        waiters_head = waiter;
        waiter = temp;
      } while (waiter != nullptr);
    }

    assert(waiters_head != nullptr);
    waiters_ = waiters_head->next;

    // Resume the next waiter
    auto next_handle = waiters_head->handle;
    auto next_exec = waiters_head->exec;

    if (next_exec) {
      next_exec->dispatch([next_handle]() {
        next_handle.resume();
      });
    } else {
      next_handle.resume();
    }
  }

 private:
  // Special value for state_ that indicates the mutex is not locked
  void* unlocked_state() noexcept {
    return this;
  }

  // Try to lock the mutex asynchronously
  // Returns true if the coroutine should suspend (lock not acquired)
  // Returns false if the lock was acquired synchronously (don't suspend)
  bool lock_async_impl(waiter_node* awaiter) {
    void* old_value = state_.load(std::memory_order_relaxed);
    while (true) {
      if (old_value == unlocked_state()) {
        // Mutex looks unlocked, try to acquire it synchronously
        void* new_value = nullptr;
        if (state_.compare_exchange_weak(old_value, new_value, std::memory_order_acquire, std::memory_order_relaxed)) {
          // Acquired synchronously, don't suspend
          return false;
        }
      } else {
        // Mutex is locked, try to queue this waiter to the list
        void* new_value = awaiter;
        awaiter->next = static_cast<waiter_node*>(old_value);
        if (state_.compare_exchange_weak(old_value, new_value, std::memory_order_release, std::memory_order_relaxed)) {
          // Queued waiter successfully, should suspend
          return true;
        }
      }
    }
  }

  // This contains either:
  // - this    => Not locked (unlocked_state)
  // - nullptr => Locked, no newly queued waiters (empty list of waiters)
  // - other   => Pointer to first waiter_node* in a linked-list of newly
  //              queued waiters in LIFO order
  std::atomic<void*> state_;

  // Linked-list of waiters in FIFO order
  // Only the current lock holder is allowed to access this member
  waiter_node* waiters_;

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
      if (mutex_) {
        mutex_->unlock();
        mutex_ = nullptr;
      }
    }

    // Move semantics - transfer ownership
    lock_guard(lock_guard&& other) noexcept : mutex_(other.mutex_) {
      other.mutex_ = nullptr;
    }

    // Disable copy
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

   private:
    mutex_t* mutex_;
  };
};

// Type aliases for convenience
using mutex = mutex_t;

}  // namespace coro
