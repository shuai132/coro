#pragma once

#include "executor_base.hpp"

namespace coro {

class executor_poll : public executor_base {
 public:
  executor_poll() = default;
  ~executor_poll() override = default;

  executor_poll(const executor_poll&) = delete;
  executor_poll(executor_poll&&) = delete;
  executor_poll& operator=(const executor_poll&) = delete;
  executor_poll& operator=(executor_poll&&) = delete;

  void poll() {
    // Only this fixed polling thread initializes the token; the check needs no acquire ordering.
    if (!running_thread_token_.load(std::memory_order_relaxed)) {
      running_thread_token_.store(current_thread_token(), std::memory_order_release);
    }

    auto task = get_next_task();
    if (task.callable()) {
      task();
    }
  }
};

}  // namespace coro
