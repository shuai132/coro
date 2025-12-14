#pragma once

#include "executor_basic_task.hpp"

namespace coro {

class executor_poll : public executor_basic_task {
 public:
  executor_poll() = default;
  ~executor_poll() override = default;

  void poll() {
    if (!running_thread_id_.load(std::memory_order_acquire).has_value()) {
      running_thread_id_.store(std::this_thread::get_id(), std::memory_order_release);
    }

    auto task_opt = get_next_task();
    if (task_opt.has_value()) {
      task_opt.value()();
    }
  }
};

}  // namespace coro
