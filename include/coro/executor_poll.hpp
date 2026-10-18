#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "executor_base.hpp"

namespace coro {

class executor_poll : public executor_base {
 protected:
  std::atomic<std::uintptr_t> running_thread_token_{0};

  static std::uintptr_t current_thread_token() noexcept {
    // Each live thread has distinct storage; compare only its address, without hashing or dynamic initialization.
    static thread_local unsigned char token;
    return reinterpret_cast<std::uintptr_t>(&token);
  }

 public:
  executor_poll() = default;
  ~executor_poll() override = default;

  executor_poll(const executor_poll&) = delete;
  executor_poll(executor_poll&&) = delete;
  executor_poll& operator=(const executor_poll&) = delete;
  executor_poll& operator=(executor_poll&&) = delete;

  // Call only from one fixed thread. Each call executes at most one ready task; after stop(), none.
  // Return the number of ready tasks invoked (0 or 1), skipping empty callbacks without waiting.
  std::size_t poll_one() {
    // Only this fixed polling thread initializes the token; the check needs no acquire ordering.
    if (!running_thread_token_.load(std::memory_order_relaxed)) {
      running_thread_token_.store(current_thread_token(), std::memory_order_release);
    }

    while (auto task = get_next_task()) {
      if (task.callable()) {
        task();
        return 1;
      }
    }
    return 0;
  }

  // After the first poll_one(), dispatch inline on that fixed thread, including between polls and after stop().
  // Other callers use post(), which rejects submissions after stop().
  void dispatch(std::function<void()> fn) override {
    if (running_thread_token_.load(std::memory_order_acquire) == current_thread_token()) {
      fn();
    } else {
      post(std::move(fn));
    }
  }
};

}  // namespace coro
