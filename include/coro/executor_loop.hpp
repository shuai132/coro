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
class executor_loop : public executor_basic_task {
 private:
  std::condition_variable condition_;

 public:
  executor_loop() = default;
  ~executor_loop() override {
    executor_loop::stop();
  };

  void run_loop() {
    running_thread_id_.store(std::hash<std::thread::id>{}(std::this_thread::get_id()), std::memory_order_release);
    for (;;) {
      std::function<void()> task;

      {
        std::unique_lock<std::mutex> lock(queue_mutex_);

        if (stop_.load(std::memory_order_acquire) && task_queue_.empty() && delayed_task_queue_.empty()) {
          return;
        }

        if (!delayed_task_queue_.empty() && delayed_task_queue_.top().execute_at <= std::chrono::steady_clock::now()) {
          task = std::move(const_cast<DelayedTask&>(delayed_task_queue_.top()).task);
          delayed_task_queue_.pop();
        } else if (!task_queue_.empty()) {
          task = std::move(task_queue_.front());
          task_queue_.pop();
        } else {
          if (delayed_task_queue_.empty()) {
            condition_.wait(lock, [this] {
              return stop_.load(std::memory_order_acquire) || !task_queue_.empty() ||
                     (!delayed_task_queue_.empty() && delayed_task_queue_.top().execute_at <= std::chrono::steady_clock::now());
            });
          } else {
            auto next_wakeup = delayed_task_queue_.top().execute_at;
            condition_.wait_until(lock, next_wakeup, [this] {
              return stop_.load(std::memory_order_acquire) || !task_queue_.empty() ||
                     (!delayed_task_queue_.empty() && delayed_task_queue_.top().execute_at <= std::chrono::steady_clock::now());
            });
          }

          if (stop_.load(std::memory_order_acquire) && task_queue_.empty() && delayed_task_queue_.empty()) {
            return;
          }

          if (!task_queue_.empty()) {
            task = std::move(task_queue_.front());
            task_queue_.pop();
          } else if (!delayed_task_queue_.empty() && delayed_task_queue_.top().execute_at <= std::chrono::steady_clock::now()) {
            task = std::move(const_cast<DelayedTask&>(delayed_task_queue_.top()).task);
            delayed_task_queue_.pop();
          }
        }
      }

      if (task) {
        task();
      }
    }
  }

  void post_delayed_ns(std::function<void()> fn, const uint64_t delay_ns) override {
    executor_basic_task::post_delayed_ns(std::move(fn), delay_ns);
    condition_.notify_one();
  }

  void post(std::function<void()> fn) override {
    executor_basic_task::post(std::move(fn));
    condition_.notify_one();
  }

  void stop() override {
    executor_basic_task::stop();
    condition_.notify_one();
  }
};
}  // namespace coro
