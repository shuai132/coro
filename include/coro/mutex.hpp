#pragma once

#include <atomic>
#include <mutex>
#include <queue>

#include "coro/coro.hpp"

namespace coro {

struct mutex {
  mutex() : locked_(false) {}

  bool is_locked() const {
    return locked_.load();
  }

  auto lock() {
    struct lock_awaitable {
      mutex* m_;

      bool await_ready() noexcept {
        // Try to acquire the lock atomically
        bool expected = false;
        return m_->locked_.compare_exchange_strong(expected, true);
      }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) {
        std::unique_lock<std::mutex> lock(m_->internal_mutex_);
        if (!m_->locked_.load()) {
          // The lock is available, try to acquire it atomically
          bool expected = false;
          if (m_->locked_.compare_exchange_strong(expected, true)) {
            // Successfully acquired the lock, continue execution
            return h;
          }
        }
        // Either the lock was already taken or we just failed to acquire it
        // Add this coroutine to the waiting queue
        m_->waiting_queue_.push(h);
        // Return noop_coroutine to suspend the current coroutine
        return std::noop_coroutine();
      }

      auto await_resume() {
        // Return a RAII-style guard that releases the lock when destroyed
        return lock_guard{m_};
      }
    };

    return lock_awaitable{this};
  }

  void unlock() {
    std::unique_lock<std::mutex> lock(internal_mutex_);
    // Mark the lock as free
    locked_.store(false);

    if (!waiting_queue_.empty()) {
      auto next_coro = waiting_queue_.front();
      waiting_queue_.pop();
      // Mark the lock as taken by the next coroutine
      locked_.store(true);
      lock.unlock();       // Release internal lock before resuming
      next_coro.resume();  // Resume the next coroutine waiting for the lock
    } else {
      lock.unlock();  // Just release the internal lock
    }
  }

 private:
  std::atomic<bool> locked_;
  mutable std::mutex internal_mutex_;  // Internal mutex to protect the waiting queue
  std::queue<std::coroutine_handle<>> waiting_queue_;

 public:
  // RAII-style lock guard that unlocks when destroyed
  class lock_guard {
   public:
    explicit lock_guard(mutex* m) : mutex_(m) {}

    ~lock_guard() {
      if (mutex_) {
        mutex_->unlock();
      }
    }

    // Move semantics - transfer ownership
    lock_guard(lock_guard&& other) noexcept : mutex_(other.mutex_) {
      other.mutex_ = nullptr;
    }

    // Disable copy
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

    mutex* mutex_;
  };
};

}  // namespace coro
