#pragma once

#include <condition_variable>
#include <mutex>

#include "executor_poll.hpp"

namespace coro {
class executor_loop : public executor_poll {
 private:
  std::condition_variable condition_;
  bool waiting_ = false;  // Protected by queue_mutex_, including the transition into wait().

 protected:
  void notify_waiters(bool stopping) override {
    if (!waiting_) {
      return;
    }
    if (stopping) {
      condition_.notify_all();
    } else {
      condition_.notify_one();
    }
  }

 public:
  executor_loop() = default;
  ~executor_loop() override {
    executor_loop::stop();
  }

  executor_loop(const executor_loop&) = delete;
  executor_loop(executor_loop&&) = delete;
  executor_loop& operator=(const executor_loop&) = delete;
  executor_loop& operator=(executor_loop&&) = delete;

  // Like poll_one(), call only from one fixed thread. stop() wakes the loop without draining queued work.
  void run_loop() {
    for (;;) {
      if (poll_one()) {
        continue;
      }

      // Recheck under the posting lock so a submission cannot be lost between polling and waiting.
      std::unique_lock<std::mutex> lock(queue_mutex_);
      if (stop_.load(std::memory_order_acquire)) {
        return;
      }
      if (!task_queue_.empty()) {
        continue;
      }
      waiting_ = true;
      if (delayed_task_queue_.empty()) {
        condition_.wait(lock);
      } else {
        auto next_wakeup = delayed_task_queue_.top().execute_at;
        condition_.wait_until(lock, next_wakeup);
      }
      waiting_ = false;
    }
  }
};
}  // namespace coro
