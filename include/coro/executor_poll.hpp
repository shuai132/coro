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
      running_thread_id_.store(std::hash<std::thread::id>{}(std::this_thread::get_id()), std::memory_order_release);
    }

    auto task_opt = get_next_task();
    if (task_opt.has_value()) {
      task_opt.value()();
    }
  }
};

}  // namespace coro
