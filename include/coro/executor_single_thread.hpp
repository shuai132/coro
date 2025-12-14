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
class executor_single_thread : public executor_basic_task {
 private:
  std::condition_variable condition_;

 public:
  executor_single_thread() = default;
  ~executor_single_thread() override {
    executor_single_thread::stop();
  };

  void run_loop() {
    running_thread_id_.store(std::this_thread::get_id(), std::memory_order_release);
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

  void post(std::function<void()> f) override {
    executor_basic_task::post(std::move(f));
    condition_.notify_one();
  }

  void post_delayed(std::function<void()> fn, const uint32_t delay) override {
    auto execute_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      delayed_task_queue_.push({execute_at, std::move(fn)});
    }
    condition_.notify_one();
  }

  void stop() override {
    stop_.store(true, std::memory_order_release);
    condition_.notify_one();
  }
};
}  // namespace coro
