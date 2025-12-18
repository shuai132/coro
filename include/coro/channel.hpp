#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <queue>

#include "coro/coro.hpp"

namespace coro {

// Dummy mutex for single-threaded scenarios (no overhead)
struct dummy_mutex {
  constexpr void lock() noexcept {}
  constexpr void unlock() noexcept {}
};

template <typename T, typename MUTEX = std::mutex>
struct channel {
  explicit channel(size_t capacity = 0) : capacity_(capacity) {}

  ~channel() {
    close();
  }

  struct send_awaitable {
    channel* ch_;
    T value_;
    executor* exec_ = nullptr;

    bool await_ready() const noexcept {
      std::lock_guard<MUTEX> lock(ch_->mutex_);
      if (ch_->closed_.load(std::memory_order_acquire)) {
        return true;  // Will return false in await_resume
      }

      // Always suspend to ensure proper synchronization
      // This allows await_suspend to handle all cases uniformly
      return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      exec_ = h.promise().executor_;

      executor* recv_exec = nullptr;
      std::coroutine_handle<> recv_h;

      {
        std::lock_guard<MUTEX> lock(ch_->mutex_);

        // Check if there's a waiting receiver for direct transfer
        if (!ch_->recv_queue_.empty()) {
          auto [recv_handle, recv_executor] = ch_->recv_queue_.front();
          ch_->recv_queue_.pop();
          recv_h = recv_handle;
          recv_exec = recv_executor;

          // Store the value for the receiver
          ch_->pending_value_ = std::move(value_);

          // Resume the receiver outside the lock
          // Sender will NOT suspend - it can proceed immediately
        }
        // Check if buffer has space
        else if (ch_->capacity_ > 0 && ch_->buffer_.size() < ch_->capacity_) {
          // Put data in buffer, sender can proceed immediately
          ch_->buffer_.push(std::move(value_));
          return false;  // Don't suspend
        }
        else {
          // No receiver available and buffer is full, wait in send queue
          ch_->send_queue_.push(std::make_tuple(h, std::move(value_), exec_));
          return true;  // Suspend
        }
      }

      // Resume the receiver outside the lock via its executor
      if (recv_h) {
        if (recv_exec) {
          recv_exec->dispatch([recv_h]() {
            recv_h.resume();
          });
        } else {
          recv_h.resume();
        }
      }

      // Don't suspend - sender can proceed immediately
      return false;
    }

    bool await_resume() {
      if (ch_->closed_.load(std::memory_order_acquire)) {
        return false;  // Send failed, channel is closed
      }
      // Send succeeded
      return true;
    }
  };

  auto send(T value) {
    return send_awaitable{this, std::move(value)};
  }

  struct recv_awaitable {
    channel* ch_;
    executor* exec_ = nullptr;

    bool await_ready() const noexcept {
      std::lock_guard<MUTEX> lock(ch_->mutex_);
      if (ch_->closed_.load(std::memory_order_acquire) && ch_->buffer_.empty()) {
        return true;  // Will return nullopt in await_resume
      }

      // Check if buffer has data
      if (!ch_->buffer_.empty()) {
        return true;  // Can receive immediately from buffer
      }

      // Need to suspend to wait for sender or check waiting senders
      return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      exec_ = h.promise().executor_;

      std::coroutine_handle<> sender_h;
      executor* sender_exec = nullptr;

      {
        std::lock_guard<MUTEX> lock(ch_->mutex_);

        // Check if there's a waiting sender for direct transfer
        if (!ch_->send_queue_.empty()) {
          auto [send_handle, value, send_executor] = std::move(ch_->send_queue_.front());
          ch_->send_queue_.pop();
          sender_h = send_handle;
          sender_exec = send_executor;

          // Store the value for this receiver
          ch_->pending_value_ = std::move(value);

          // Resume the sender outside the lock
          // Receiver will NOT suspend - it can proceed immediately with the value
        }
        else {
          // No sender available, wait in recv queue
          ch_->recv_queue_.push(std::make_pair(h, exec_));
          return true;  // Suspend
        }
      }

      // Resume the sender outside the lock via its executor
      if (sender_h) {
        if (sender_exec) {
          sender_exec->dispatch([sender_h]() {
            sender_h.resume();
          });
        } else {
          sender_h.resume();
        }
      }

      // Don't suspend - receiver can proceed immediately
      return false;
    }

    std::optional<T> await_resume() {
      std::coroutine_handle<> sender_to_resume;
      executor* sender_exec = nullptr;
      bool should_resume_sender = false;
      std::optional<T> result;

      {
        std::lock_guard<MUTEX> lock(ch_->mutex_);

        if (ch_->closed_.load(std::memory_order_acquire) && ch_->buffer_.empty() && !ch_->pending_value_.has_value()) {
          return std::nullopt;  // Channel is closed and no data available
        }

        // Check if we have a pending value from direct transfer
        if (ch_->pending_value_.has_value()) {
          result = std::move(ch_->pending_value_.value());
          ch_->pending_value_.reset();
          return result;
        }

        // Check buffer
        if (!ch_->buffer_.empty()) {
          result = std::move(ch_->buffer_.front());
          ch_->buffer_.pop();

          // After reading from buffer, check if there are waiting senders
          if (!ch_->send_queue_.empty()) {
            auto [sender_h, sender_value, send_executor] = std::move(ch_->send_queue_.front());
            ch_->send_queue_.pop();
            ch_->buffer_.push(std::move(sender_value));
            sender_to_resume = sender_h;
            sender_exec = send_executor;
            should_resume_sender = true;
          }
        } else {
          // This should not happen in normal flow
          return std::nullopt;
        }
      }

      // Resume sender outside the lock via its executor
      if (should_resume_sender) {
        if (sender_exec) {
          sender_exec->dispatch([sender_to_resume]() {
            sender_to_resume.resume();
          });
        } else {
          sender_to_resume.resume();
        }
      }

      return result;
    }
  };

  auto recv() {
    return recv_awaitable{this};
  }

  bool is_closed() const {
    return closed_.load(std::memory_order_acquire);
  }

  void close() {
    std::vector<std::pair<std::coroutine_handle<>, executor*>> senders_to_resume;
    std::vector<std::pair<std::coroutine_handle<>, executor*>> receivers_to_resume;

    {
      std::lock_guard<MUTEX> lock(mutex_);
      bool expected = false;
      if (!closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;  // Already closed
      }

      // Collect all waiting senders and receivers
      while (!send_queue_.empty()) {
        auto [h, val, exec] = std::move(send_queue_.front());
        send_queue_.pop();
        senders_to_resume.push_back(std::make_pair(h, exec));
      }
      while (!recv_queue_.empty()) {
        receivers_to_resume.push_back(recv_queue_.front());
        recv_queue_.pop();
      }
    }

    // Wake up all waiting senders and receivers outside the lock
    for (auto [h, exec] : senders_to_resume) {
      if (exec) {
        exec->dispatch([h]() {
          h.resume();
        });
      } else {
        h.resume();
      }
    }
    for (auto [h, exec] : receivers_to_resume) {
      if (exec) {
        exec->dispatch([h]() {
          h.resume();
        });
      } else {
        h.resume();
      }
    }
  }

  bool empty() const {
    std::lock_guard<MUTEX> lock(mutex_);
    return buffer_.empty();
  }

  bool full() const {
    std::lock_guard<MUTEX> lock(mutex_);
    return capacity_ > 0 && buffer_.size() >= capacity_;
  }

  size_t size() const {
    std::lock_guard<MUTEX> lock(mutex_);
    return buffer_.size();
  }

  size_t capacity() const {
    return capacity_;
  }

 private:
  size_t capacity_;
  mutable MUTEX mutex_;  // Protects all mutable state
  std::queue<T> buffer_;                                          // For buffered channels
  std::queue<std::tuple<std::coroutine_handle<>, T, executor*>> send_queue_;  // Waiting senders with values and executors
  std::queue<std::pair<std::coroutine_handle<>, executor*>> recv_queue_;  // Waiting receivers with executors
  std::optional<T> pending_value_;                                // Temporary storage for direct value passing
  std::atomic<bool> closed_{false};
};

// Aliases for convenience
template <typename T>
using unbuffered_channel = channel<T, std::mutex>;

template <typename T>
using mt_channel = channel<T, std::mutex>;  // Multi-threaded channel

template <typename T>
using st_channel = channel<T, dummy_mutex>;  // Single-threaded channel (no lock overhead)

}  // namespace coro
