#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <utility>

#include "executor.hpp"

namespace coro {
class executor_basic_task : public coro::executor {
 protected:
  struct DelayedTask {
    std::chrono::steady_clock::time_point execute_at;
    std::function<void()> task;
  };

  struct DelayedTaskCompare {
    bool operator()(const DelayedTask& lhs, const DelayedTask& rhs) const {
      return lhs.execute_at > rhs.execute_at;
    };
  };

 protected:
  std::mutex queue_mutex_;
  std::queue<std::function<void()>> task_queue_;
  std::priority_queue<DelayedTask, std::vector<DelayedTask>, DelayedTaskCompare> delayed_task_queue_;
  std::atomic<size_t> running_thread_id_;
  std::atomic<bool> stop_{false};

 protected:
  std::optional<std::function<void()>> get_next_task() {
    if (stop_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!delayed_task_queue_.empty() && delayed_task_queue_.top().execute_at <= std::chrono::steady_clock::now()) {
      auto task = std::move(const_cast<DelayedTask&>(delayed_task_queue_.top()).task);
      delayed_task_queue_.pop();
      return task;
    } else if (!task_queue_.empty()) {
      auto task = std::move(task_queue_.front());
      task_queue_.pop();
      return task;
    }
    return std::nullopt;
  }

  bool has_pending_tasks() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!delayed_task_queue_.empty() && delayed_task_queue_.top().execute_at <= std::chrono::steady_clock::now()) {
      return true;
    }
    return !task_queue_.empty();
  }

 public:
  executor_basic_task() = default;

  ~executor_basic_task() override {
    executor_basic_task::stop();
  }

  void dispatch(std::function<void()> fn) override {
    if (running_thread_id_.load(std::memory_order_acquire) == std::hash<std::thread::id>{}(std::this_thread::get_id())) {
      fn();
    } else {
      post(std::move(fn));
    }
  }

  void post_delayed(std::function<void()> fn, const uint64_t delay_ns) override {
    auto execute_at = std::chrono::steady_clock::now() + std::chrono::nanoseconds(delay_ns);
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      delayed_task_queue_.push({execute_at, std::move(fn)});
    }
  }

  virtual void post(std::function<void()> f) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    task_queue_.emplace(std::move(f));
  }

  void stop() override {
    stop_.store(true, std::memory_order_release);
  }

  bool stopped() {
    return stop_.load(std::memory_order_acquire);
  }
};

}  // namespace coro
