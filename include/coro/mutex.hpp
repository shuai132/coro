#pragma once

#include <queue>

#include "coro/coro.hpp"

namespace coro {

struct mutex {
  mutex() : locked_(false) {}

  bool is_locked() const {
    return locked_;
  }

  auto lock() {
    struct lock_awaitable {
      mutex* m_;

      bool await_ready() noexcept {
        // In a single-threaded environment, if the lock is free, acquire it atomically and return true to prevent suspension
        if (!m_->locked_) {
          m_->locked_ = true;
          return true;  // Don't suspend, we have the lock
        }
        return false;  // Suspend, as the lock is already taken
      }

      void await_suspend(std::coroutine_handle<> h) {
        // If we reach here, it means await_ready() returned false, so the lock is definitely held by another coroutine
        m_->waiting_queue_.push(h);
        // The coroutine will remain suspended until another coroutine calls unlock()
      }

      auto await_resume() {
        // Return a RAII-style guard that releases the lock when destroyed
        return lock_guard{m_};
      }
    };

    return lock_awaitable{this};
  }

  void unlock() {
    // mark the lock as free
    locked_ = false;

    if (!waiting_queue_.empty()) {
      auto next_coro = waiting_queue_.front();
      waiting_queue_.pop();
      locked_ = true;      // mark the lock as taken by the next coroutine
      next_coro.resume();  // resume the next coroutine waiting for the lock
    }
  }

 private:
  bool locked_;
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

    // move semantics - transfer ownership
    lock_guard(lock_guard&& other) noexcept : mutex_(other.mutex_) {
      other.mutex_ = nullptr;
    }

    // disable copy
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

    mutex* mutex_;
  };
};

}  // namespace coro
