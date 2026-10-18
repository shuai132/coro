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
    if (!running_thread_id_.load(std::memory_order_acquire)) {
      running_thread_id_.store(current_thread_hash(), std::memory_order_release);
    }

    auto task = get_next_task();
    if (task.callable()) {
      task();
    }
  }
};

}  // namespace coro
