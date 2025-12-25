#pragma once

#include <coroutine>
#include <mutex>

#include "coro/coro.hpp"
#include "coro/dummy_mutex.hpp"

namespace coro {

// Coroutine-safe counting semaphore
// Controls access to a shared resource by maintaining a count of available permits
//
// Template parameter MUTEX controls internal thread safety:
//   - std::mutex: Thread-safe for multithreaded use (default)
//   - dummy_mutex: No lock overhead for single-threaded use
template <typename MUTEX = std::mutex>
struct counting_semaphore_t {
 private:
  // Intrusive list node for waiting coroutines
  struct waiter_node {
    std::coroutine_handle<> handle;
    executor* exec{};
    waiter_node* next{};
    int desired{1};  // Number of permits desired
  };

 public:
  // Construct a semaphore with the given number of permits
  // least_max_value is typically the maximum value the semaphore will use
  explicit counting_semaphore_t(int desired, int least_max_value = INT_MAX)
      : counter_(desired), max_value_(least_max_value), head_(nullptr), tail_(nullptr) {}

  counting_semaphore_t(const counting_semaphore_t&) = delete;
  counting_semaphore_t(counting_semaphore_t&&) = delete;
  counting_semaphore_t& operator=(const counting_semaphore_t&) = delete;
  counting_semaphore_t& operator=(counting_semaphore_t&&) = delete;

  // Acquire n permits (default 1)
  // Suspends if not enough permits are available
  struct acquire_awaitable {
    counting_semaphore_t* sem_;
    int desired_;
    waiter_node node_{};

    explicit acquire_awaitable(counting_semaphore_t* sem, int n = 1) : sem_(sem), desired_(n) {
      node_.desired = n;
    }

    bool await_ready() noexcept {
      std::lock_guard<MUTEX> lock(sem_->mutex_);
      if (sem_->counter_ >= desired_) {
        sem_->counter_ -= desired_;
        return true;  // Don't suspend
      }
      return false;  // Need to suspend
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      node_.handle = h;
      node_.exec = h.promise().executor_;
      node_.next = nullptr;

      {
        std::lock_guard<MUTEX> lock(sem_->mutex_);

        // Double check after acquiring lock
        if (sem_->counter_ >= desired_) {
          sem_->counter_ -= desired_;
          return false;  // Don't suspend
        }

        // Add to waiting list
        if (sem_->tail_) {
          sem_->tail_->next = &node_;
        } else {
          sem_->head_ = &node_;
        }
        sem_->tail_ = &node_;
      }

      return true;  // Suspend
    }

    void await_resume() noexcept {
      // Permits acquired
    }
  };

  // Acquire n permits (default 1)
  // Usage: co_await sem.acquire();
  auto acquire(int n = 1) {
    return acquire_awaitable{this, n};
  }

  // Try to acquire n permits without blocking
  // Returns true if successful, false otherwise
  bool try_acquire(int n = 1) noexcept {
    std::lock_guard<MUTEX> lock(mutex_);
    if (counter_ >= n) {
      counter_ -= n;
      return true;
    }
    return false;
  }

  // Release n permits (default 1)
  // Wakes up waiting coroutines if possible
  void release(int n = 1) {
    waiter_node* nodes_to_resume = nullptr;  // Head of resume list

    {
      std::lock_guard<MUTEX> lock(mutex_);
      counter_ += n;

      // Wake up waiting coroutines that can now be satisfied
      waiter_node* prev = nullptr;
      waiter_node* node = head_;

      while (node) {
        if (counter_ >= node->desired) {
          // This waiter can be satisfied
          counter_ -= node->desired;

          // Remove from waiting list
          if (prev) {
            prev->next = node->next;
          } else {
            head_ = node->next;
          }

          if (node == tail_) {
            tail_ = prev;
          }

          waiter_node* next = node->next;

          // Add to resume list (prepend for O(1))
          node->next = nodes_to_resume;
          nodes_to_resume = node;

          node = next;
        } else {
          prev = node;
          node = node->next;
        }
      }
    }

    // Resume all satisfied waiters outside the lock
    waiter_node* waiter = nodes_to_resume;
    while (waiter) {
      waiter_node* next = waiter->next;
      if (waiter->exec) {
        waiter->exec->dispatch([handle = waiter->handle]() {
          handle.resume();
        });
      } else {
        waiter->handle.resume();
      }
      waiter = next;
    }
  }

  // Get current number of available permits
  int available() const noexcept {
    std::lock_guard<MUTEX> lock(mutex_);
    return counter_;
  }

  // Get maximum value
  int max() const noexcept {
    return max_value_;
  }

 private:
  int counter_;          // Current number of available permits
  int max_value_;        // Maximum value
  mutable MUTEX mutex_;  // Internal mutex
  waiter_node* head_;    // Head of waiting list
  waiter_node* tail_;    // Tail of waiting list
};

// Binary semaphore (special case with max value of 1)
template <typename MUTEX = std::mutex>
struct binary_semaphore_t {
 public:
  explicit binary_semaphore_t(int desired) : sem_(desired, 1) {}

  auto acquire() {
    return sem_.acquire(1);
  }

  bool try_acquire() noexcept {
    return sem_.try_acquire(1);
  }

  void release() {
    sem_.release(1);
  }

  int available() const noexcept {
    return sem_.available();
  }

 private:
  counting_semaphore_t<MUTEX> sem_;
};

// Type aliases for convenience
using counting_semaphore = counting_semaphore_t<std::mutex>;
using counting_semaphore_mt = counting_semaphore_t<std::mutex>;
using counting_semaphore_st = counting_semaphore_t<dummy_mutex>;

using binary_semaphore = binary_semaphore_t<std::mutex>;
using binary_semaphore_mt = binary_semaphore_t<std::mutex>;
using binary_semaphore_st = binary_semaphore_t<dummy_mutex>;

}  // namespace coro
