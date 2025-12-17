#pragma once

#include <coroutine>

#include "coro/coro.hpp"

namespace coro {

struct mutex {
 private:
  // Intrusive list node stored in the awaitable (which is part of coroutine frame)
  struct waiter_node {
    std::coroutine_handle<> handle;
    waiter_node* next{nullptr};
  };

 public:
  mutex() : locked_(false), head_(nullptr), tail_(nullptr) {}

  bool is_locked() const {
    return locked_;
  }

  // Low-level lock operation, must be paired with unlock()
  // Usage: co_await mutex.lock();
  auto lock() {
    struct lock_awaitable {
      mutex* m_;
      waiter_node node_{};  // Node lives as long as the awaitable (part of coroutine frame)

      bool await_ready() noexcept {
        // In a single-threaded environment, if the lock is free, acquire it
        if (!m_->locked_) {
          m_->locked_ = true;
          return true;  // Don't suspend, we have the lock
        }
        return false;  // Suspend, as the lock is already taken
      }

      bool await_suspend(std::coroutine_handle<> h) {
        // Initialize the node
        node_.handle = h;
        node_.next = nullptr;

        // Add to the waiting list
        if (m_->tail_) {
          m_->tail_->next = &node_;
        } else {
          m_->head_ = &node_;
        }
        m_->tail_ = &node_;

        // Return true to suspend
        return true;
      }

      void await_resume() noexcept {
        // Lock acquired, nothing to return
      }
    };

    return lock_awaitable{this};
  }

  // RAII-style scoped lock that returns a lock_guard
  // Usage: auto guard = co_await mutex.scoped_lock();
  auto scoped_lock() {
    struct scoped_lock_awaitable {
      mutex* m_;
      waiter_node node_{};  // Node lives as long as the awaitable (part of coroutine frame)

      bool await_ready() noexcept {
        // In single-threaded coroutine environment, check and acquire the lock
        if (!m_->locked_) {
          m_->locked_ = true;
          return true;  // Successfully acquired the lock, don't suspend
        }
        return false;  // Lock is held, need to suspend
      }

      bool await_suspend(std::coroutine_handle<> h) {
        // Initialize the node
        node_.handle = h;
        node_.next = nullptr;

        // Add to the waiting list
        if (m_->tail_) {
          m_->tail_->next = &node_;
        } else {
          m_->head_ = &node_;
        }
        m_->tail_ = &node_;

        // Return true to suspend
        return true;
      }

      auto await_resume() {
        // Return a RAII-style guard that releases the lock when destroyed
        return lock_guard{m_};
      }
    };

    return scoped_lock_awaitable{this};
  }

  void unlock() {
    // Check if there are waiting coroutines
    if (head_) {
      auto* node = head_;
      auto next_handle = node->handle;

      // Move head to next BEFORE resuming
      // This is critical: we must update the list before resume
      // because the node will be destroyed when the coroutine runs
      head_ = node->next;
      if (!head_) {
        tail_ = nullptr;
      }

      // The lock remains held (locked_ stays true), transfer to next coroutine
      // When next_handle resumes, it will return from co_await and the node
      // will be destroyed, but we've already updated head_ so it's safe
      next_handle.resume();
    } else {
      // No waiters, release the lock
      locked_ = false;
    }
  }

 private:
  bool locked_;
  waiter_node* head_;  // Head of waiting list
  waiter_node* tail_;  // Tail of waiting list

 public:
  // RAII-style lock guard that unlocks when destroyed
  class lock_guard {
   public:
    explicit lock_guard(mutex* m) : mutex_(m) {}

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

    // move semantics - transfer ownership
    lock_guard(lock_guard&& other) noexcept : mutex_(other.mutex_) {
      other.mutex_ = nullptr;
    }

    // disable copy
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

   private:
    mutex* mutex_;
  };
};

}  // namespace coro
