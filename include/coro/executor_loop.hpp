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
  // A thread can enter nested loops; execution context lasts only until each run_loop returns.
  struct running_loop_context {
    executor_loop* loop_;
    running_loop_context* previous_;
    inline static thread_local running_loop_context* current_ = nullptr;

    explicit running_loop_context(executor_loop* loop) noexcept : loop_(loop), previous_(current_) {
      current_ = this;
    }
    ~running_loop_context() {
      current_ = previous_;
    }
    running_loop_context(const running_loop_context&) = delete;
    running_loop_context& operator=(const running_loop_context&) = delete;

    static bool contains(const executor_loop* loop) noexcept {
      for (auto* context = current_; context; context = context->previous_) {
        if (context->loop_ == loop) {
          return true;
        }
      }
      return false;
    }
  };

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
    running_loop_context context{this};
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

  void dispatch(std::function<void()> fn) override {
    if (running_loop_context::contains(this)) {
      fn();
    } else {
      post(std::move(fn));
    }
  }

  void post(std::function<void()> fn) override {
    executor_base::post(std::move(fn));
    condition_.notify_one();
  }

  void post_continuation(std::function<void()> fn) override {
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      // Already accepted work may resume suspended operations while this loop drains after stop().
      if (stop_.load(std::memory_order_acquire) && !running_loop_context::contains(this)) {
        return;
      }
      task_queue_.emplace(std::move(fn));
    }
    condition_.notify_one();
  }

  void stop() override {
    executor_base::stop();
    condition_.notify_one();
  }
};
}  // namespace coro
