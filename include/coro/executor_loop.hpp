#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <utility>

#include "executor_poll.hpp"

namespace coro {
class executor_loop : public executor_base {
 private:
  std::condition_variable condition_;

 public:
  executor_loop() = default;
  ~executor_loop() override {
    executor_loop::stop();
  };

  executor_loop(const executor_loop&) = delete;
  executor_loop(executor_loop&&) = delete;
  executor_loop& operator=(const executor_loop&) = delete;
  executor_loop& operator=(executor_loop&&) = delete;

  void run_loop() {
    running_thread_id_.store(std::hash<std::thread::id>{}(std::this_thread::get_id()), std::memory_order_release);
    for (;;) {
      std::function<void()> task;

      {
        std::unique_lock<std::mutex> lock(queue_mutex_);

        if (stop_.load(std::memory_order_acquire) && task_queue_.empty() && delayed_task_queue_.empty()) {
          return;
        }

        if (auto next = get_next_task_locked()) {
          task = std::move(*next);
        } else {
          if (delayed_task_queue_.empty()) {
            condition_.wait(lock, [this] {
              return stop_.load(std::memory_order_acquire) || !task_queue_.empty() || !delayed_task_queue_.empty();
            });
          } else {
            auto next_wakeup = delayed_task_queue_.top().execute_at;
            condition_.wait_until(lock, next_wakeup, [this, next_wakeup] {
              return !task_queue_.empty() || (!delayed_task_queue_.empty() && delayed_task_queue_.top().execute_at < next_wakeup);
            });
          }

          continue;
        }
      }

      if (task) {
        task();
      }
    }
  }

  void post_delayed_ns(std::function<void()> fn, const uint64_t delay_ns) override {
    executor_base::post_delayed_ns(std::move(fn), delay_ns);
    condition_.notify_one();
  }

  void post(std::function<void()> fn) override {
    executor_base::post(std::move(fn));
    condition_.notify_one();
  }

  void stop() override {
    executor_base::stop();
    condition_.notify_one();
  }
};
}  // namespace coro
