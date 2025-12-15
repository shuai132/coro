#pragma once

#include <optional>
#include <queue>

#ifndef CORO_DISABLE_EXCEPTION
#include <stdexcept>
#endif

#include "coro/coro.hpp"

namespace coro {

template <typename T>
struct channel {
  explicit channel(size_t capacity = 0) : capacity_(capacity) {}

  ~channel() {
    close();
  }

  auto send(T value) {
    struct send_awaitable {
      channel* ch_;
      T value_;

      bool await_ready() const noexcept {
        if (ch_->closed_) {
          return true;  // Will throw in await_resume
        }

        // Always suspend to ensure proper synchronization
        // This allows await_suspend to handle all cases uniformly
        return false;
      }

      void await_suspend(std::coroutine_handle<> h) {
        // Check if there's a waiting receiver for direct transfer
        if (!ch_->recv_queue_.empty()) {
          auto recv_h = ch_->recv_queue_.front();
          ch_->recv_queue_.pop();

          // Store the value for the receiver
          ch_->pending_value_ = std::move(value_);

          // Resume the receiver - it will get the value and complete
          recv_h.resume();
          // Also resume the sender since the operation is complete
          h.resume();
          return;
        }

        // Check if buffer has space
        if (ch_->capacity_ > 0 && ch_->buffer_.size() < ch_->capacity_) {
          // Put data in buffer and resume sender immediately
          ch_->buffer_.push(std::move(value_));
          h.resume();
          return;
        }

        // No receiver available and buffer is full, wait in send queue
        ch_->send_queue_.push(std::make_pair(h, std::move(value_)));
      }

      void await_resume() {
        if (ch_->closed_) {
#ifndef CORO_DISABLE_EXCEPTION
          throw std::runtime_error("Channel is closed");
#else
          std::terminate();
#endif
        }
        // No additional work needed - all handling is done in await_suspend
      }
    };

    return send_awaitable{this, std::move(value)};
  }

  auto recv() {
    struct recv_awaitable {
      channel* ch_;

      bool await_ready() const noexcept {
        if (ch_->closed_ && ch_->buffer_.empty()) {
          return true;  // Will handle in await_resume
        }

        // Check if buffer has data
        if (!ch_->buffer_.empty()) {
          return true;  // Can receive immediately from buffer
        }

        // Need to suspend to wait for sender or check waiting senders
        return false;
      }

      void await_suspend(std::coroutine_handle<> h) {
        // Check if there's a waiting sender for direct transfer
        if (!ch_->send_queue_.empty()) {
          auto [sender_h, value] = std::move(ch_->send_queue_.front());
          ch_->send_queue_.pop();

          // Store the value for this receiver
          ch_->pending_value_ = std::move(value);

          // Resume the sender to complete its operation
          sender_h.resume();
          // Resume this receiver to get the value
          h.resume();
          return;
        }

        // No sender available, wait in recv queue
        ch_->recv_queue_.push(h);
      }

      T await_resume() {
        if (ch_->closed_ && ch_->buffer_.empty() && !ch_->pending_value_.has_value()) {
          throw std::runtime_error("Channel is closed and no data available");
        }

        // Check if we have a pending value from direct transfer
        if (ch_->pending_value_.has_value()) {
          T value = std::move(ch_->pending_value_.value());
          ch_->pending_value_.reset();
          return value;
        }

        // Check buffer
        if (!ch_->buffer_.empty()) {
          T value = std::move(ch_->buffer_.front());
          ch_->buffer_.pop();

          // After reading from buffer, check if there are waiting senders
          if (!ch_->send_queue_.empty()) {
            auto [sender_h, sender_value] = std::move(ch_->send_queue_.front());
            ch_->send_queue_.pop();
            ch_->buffer_.push(std::move(sender_value));
            sender_h.resume();
          }

          return value;
        }

        // This should not happen in normal flow
        throw std::runtime_error("No data available");
      }
    };

    return recv_awaitable{this};
  }

  bool is_closed() const {
    return closed_;
  }

  void close() {
    if (closed_) return;
    closed_ = true;

    // Wake up all waiting senders and receivers
    while (!send_queue_.empty()) {
      send_queue_.front().first.resume();
      send_queue_.pop();
    }
    while (!recv_queue_.empty()) {
      recv_queue_.front().resume();
      recv_queue_.pop();
    }
  }

  bool empty() const {
    return buffer_.empty();
  }

  bool full() const {
    return capacity_ > 0 && buffer_.size() >= capacity_;
  }

  size_t size() const {
    return buffer_.size();
  }

  size_t capacity() const {
    return capacity_;
  }

 private:
  size_t capacity_;
  std::queue<T> buffer_;                                          // For buffered channels
  std::queue<std::pair<std::coroutine_handle<>, T>> send_queue_;  // Waiting senders with values
  std::queue<std::coroutine_handle<>> recv_queue_;                // Waiting receivers
  std::optional<T> pending_value_;                                // Temporary storage for direct value passing
  bool closed_ = false;
};

// Specialization for unbuffered channel (capacity=0)
template <typename T>
using unbuffered_channel = channel<T>;

}  // namespace coro
