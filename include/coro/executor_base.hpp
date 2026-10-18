#pragma once

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>

#include "coro/detail/callback_cache.hpp"
#include "coro/detail/deadline.hpp"
#include "coro/detail/resume.hpp"
#include "executor.hpp"

namespace coro {
// Shared queues for built-in executors; derived types provide dispatch() and task driving.
class executor_base : public coro::executor {
 protected:
  executor_base() = default;

  struct queued_task {
    std::coroutine_handle<> handle_{};
    detail::queued_callback callback_;

    queued_task() = default;
    explicit queued_task(std::coroutine_handle<> handle) noexcept : handle_(handle) {}
    explicit queued_task(detail::queued_callback callback) noexcept : callback_(std::move(callback)) {}

    explicit operator bool() const noexcept {
      return handle_ || callback_;
    }

    bool callable() const noexcept {
      return handle_ || (callback_ && *callback_);
    }

    void operator()() {
      if (handle_) {
        detail::resume_handle(handle_);
      } else {
        (*callback_)();
      }
    }
  };

  static queued_task make_task(std::function<void()>&& fn) {
#if defined(__cpp_rtti) || defined(_CPPRTTI)
    if (auto* resume = fn.target<detail::resume_task>()) {
      return queued_task{resume->handle_};
    }
#endif
    // Moving std::function can copy a small target; keep all user code outside queue locks.
    return queued_task{detail::make_queued_callback(std::move(fn))};
  }

  struct DelayedTask {
    std::chrono::steady_clock::time_point execute_at;
    queued_task task;
  };

  struct DelayedTaskCompare {
    bool operator()(const DelayedTask& lhs, const DelayedTask& rhs) const noexcept {
      return lhs.execute_at > rhs.execute_at;
    };
  };

 protected:
  std::mutex queue_mutex_;
  std::queue<queued_task> task_queue_;
  std::priority_queue<DelayedTask, std::vector<DelayedTask>, DelayedTaskCompare> delayed_task_queue_;
  std::atomic<bool> stop_{false};
  bool prefer_delayed_{true};  // Protected by queue_mutex_.

 protected:
  // Notify before releasing queue_mutex_, so consuming the task also observes notification completion.
  virtual void notify_waiters([[maybe_unused]] bool stopping) {}

  queued_task get_next_task() {
    if (stop_.load(std::memory_order_acquire)) {
      return {};
    }

    std::lock_guard<std::mutex> lock(queue_mutex_);
    return get_next_task_locked();
  }

  // Alternate ready queues so neither can starve the other by replenishing itself.
  queued_task get_next_task_locked() {
    if (!delayed_task_queue_.empty() && delayed_task_queue_.top().execute_at <= std::chrono::steady_clock::now() &&
        (task_queue_.empty() || prefer_delayed_)) {
      auto task = std::move(const_cast<DelayedTask&>(delayed_task_queue_.top()).task);
      delayed_task_queue_.pop();
      prefer_delayed_ = false;
      return task;
    } else if (!task_queue_.empty()) {
      auto task = std::move(task_queue_.front());
      task_queue_.pop();
      prefer_delayed_ = true;
      return task;
    }
    return {};
  }

  bool has_pending_tasks() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!delayed_task_queue_.empty() && delayed_task_queue_.top().execute_at <= std::chrono::steady_clock::now()) {
      return true;
    }
    return !task_queue_.empty();
  }

 public:
  ~executor_base() override {
    executor_base::stop();
  }

  executor_base(const executor_base&) = delete;
  executor_base(executor_base&&) = delete;
  executor_base& operator=(const executor_base&) = delete;
  executor_base& operator=(executor_base&&) = delete;

  // Submissions after stop(), including coroutine resumptions, are discarded on every thread.
  // Rejected callbacks are released without invocation.
  void post(std::function<void()> fn) override {
    if (stop_.load(std::memory_order_acquire)) {
      return;
    }
    auto task = make_task(std::move(fn));
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (stop_.load(std::memory_order_acquire)) {
      return;
    }
    task_queue_.emplace(std::move(task));
    notify_waiters(false);
  }

  // Like post(), reject new timers after stop(); accepted timers keep their original deadlines.
  void post_delayed_ns(std::function<void()> fn, const uint64_t delay_ns) override {
    if (stop_.load(std::memory_order_acquire)) {
      return;
    }
    // The heap rearranges ownership pointers, leaving callback objects stationary.
    DelayedTask task{{}, make_task(std::move(fn))};
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (stop_.load(std::memory_order_acquire)) {
      return;
    }
    task.execute_at = detail::delayed_deadline(delay_ns);
    delayed_task_queue_.push(std::move(task));
    notify_waiters(false);
  }

  // Reject new posts and timers, then notify runners. executor_loop drains queued work;
  // executor_poll stops dequeuing. This neither cancels coroutines nor joins runner threads,
  // and derived dispatch() implementations may still invoke callbacks inline.
  virtual void stop() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    stop_.store(true, std::memory_order_release);
    notify_waiters(true);
  }

  bool stopped() const noexcept {
    return stop_.load(std::memory_order_acquire);
  }
};

}  // namespace coro
