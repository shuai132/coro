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
      // Check if channel is closed without suspending
      // std::lock_guard<MUTEX> lock(ch_->mutex_);
      return ch_->closed_.load(std::memory_order_acquire);
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      exec_ = h.promise().executor_;

      {
        std::unique_lock<MUTEX> lock(ch_->mutex_);

        // Check if there's a waiting receiver for direct transfer
        if (!ch_->closed_.load(std::memory_order_acquire) && !ch_->recv_queue_.empty()) {
          auto [recv_handle, recv_executor] = ch_->recv_queue_.front();
          ch_->recv_queue_.pop();
          ch_->pending_value_ = std::move(value_);

          // Resume receiver immediately with the value
          lock.unlock();
          if (recv_executor) {
            recv_executor->dispatch([recv_handle]() {
              recv_handle.resume();
            });
          } else {
            recv_handle.resume();
          }

          // Return false to continue execution without suspension
          return false;
        }

        // Check if buffer has space
        if (!ch_->closed_.load(std::memory_order_acquire) && ch_->capacity_ > 0 && ch_->buffer_.size() < ch_->capacity_) {
          // Put data in buffer, sender can proceed immediately
          ch_->buffer_.push(std::move(value_));
          return false;  // Don't suspend
        }

        if (!ch_->closed_.load(std::memory_order_acquire)) {
          // No receiver available and buffer is full, wait in send queue
          ch_->send_queue_.push(std::make_tuple(h, std::move(value_), exec_));
          return true;  // Suspend
        }
      }

      // If channel is closed, don't suspend - just return false to allow await_resume to handle it
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
      // Check if channel is closed and no data available without suspending
      // std::lock_guard<MUTEX> lock(ch_->mutex_);
      return ch_->closed_.load(std::memory_order_acquire) && ch_->buffer_.empty();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      exec_ = h.promise().executor_;

      {
        std::unique_lock<MUTEX> lock(ch_->mutex_);

        // Check if buffer has data
        if (!ch_->buffer_.empty()) {
          // Data available in buffer, don't suspend
          return false;
        }

        // Check if there's a waiting sender for direct transfer
        if (!ch_->closed_.load(std::memory_order_acquire) && !ch_->send_queue_.empty()) {
          auto [send_handle, value, send_executor] = std::move(ch_->send_queue_.front());
          ch_->send_queue_.pop();
          // Store the value for this receiver
          ch_->pending_value_ = std::move(value);

          // Resume sender immediately so it can continue
          lock.unlock();
          if (send_executor) {
            send_executor->dispatch([send_handle]() {
              send_handle.resume();
            });
          } else {
            send_handle.resume();
          }

          // Return false to continue execution without suspension
          return false;
        }

        if (!ch_->closed_.load(std::memory_order_acquire)) {
          // No data available, wait in recv queue
          ch_->recv_queue_.push(std::make_pair(h, exec_));
          return true;  // Suspend
        }
      }

      // If channel is closed, don't suspend - just return false to allow await_resume to handle it
      return false;
    }

    std::optional<T> await_resume() {
      std::unique_lock<MUTEX> lock(ch_->mutex_);

      if (ch_->closed_.load(std::memory_order_acquire) && ch_->buffer_.empty() && !ch_->pending_value_.has_value()) {
        return std::nullopt;  // Channel is closed and no data available
      }

      // Check if we have a pending value from direct transfer
      if (ch_->pending_value_.has_value()) {
        auto result = std::move(ch_->pending_value_.value());
        ch_->pending_value_.reset();
        return result;
      }

      // Check buffer
      if (!ch_->buffer_.empty()) {
        auto result = std::move(ch_->buffer_.front());
        ch_->buffer_.pop();

        // After reading from buffer, check if there are waiting senders to move to buffer
        if (!ch_->send_queue_.empty() && ch_->buffer_.size() < ch_->capacity_) {
          auto [sender_h, sender_value, sender_exec] = std::move(ch_->send_queue_.front());
          ch_->send_queue_.pop();
          ch_->buffer_.push(std::move(sender_value));

          // Resume the sender since its value is now in the buffer
          lock.unlock();
          if (sender_exec) {
            sender_exec->dispatch([sender_h]() {
              sender_h.resume();
            });
          } else {
            sender_h.resume();
          }
        }

        return result;
      }

      // This should not happen in normal flow
      return std::nullopt;
    }
  };

  auto recv() {
    return recv_awaitable{this};
  }

  bool is_closed() const {
    return closed_.load(std::memory_order_acquire);
  }

  void close() {
    // Collect handles to resume outside the lock to minimize lock time
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
  mutable MUTEX mutex_;                                                       // Protects all mutable state
  std::queue<T> buffer_;                                                      // For buffered channels
  std::queue<std::tuple<std::coroutine_handle<>, T, executor*>> send_queue_;  // Waiting senders with values and executors
  std::queue<std::pair<std::coroutine_handle<>, executor*>> recv_queue_;      // Waiting receivers with executors
  std::optional<T> pending_value_;                                            // Temporary storage for direct value passing
  std::atomic<bool> closed_{false};
};

template <typename T>
using mt_channel = channel<T, std::mutex>;  // Multi-threaded channel

template <typename T>
using st_channel = channel<T, dummy_mutex>;  // Single-threaded channel (no lock overhead)

// Aliases for convenience
template <typename T, typename M>
using unbuffered_channel = channel<T, M>;

template <typename T>
using mt_unbuffered_channel = unbuffered_channel<T, std::mutex>;

template <typename T>
using st_unbuffered_channel = unbuffered_channel<T, dummy_mutex>;

}  // namespace coro
