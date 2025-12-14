#pragma once

#include <queue>
#include <optional>

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
        // Can send immediately only if there's buffer space
        return ch_->capacity_ > 0 && ch_->buffer_.size() < ch_->capacity_;
      }

      void await_suspend(std::coroutine_handle<> h) {
        // Check if there's a waiting receiver for direct transfer
        if (!ch_->recv_queue_.empty()) {
          // Get the waiting receiver
          auto recv_h = ch_->recv_queue_.front();
          ch_->recv_queue_.pop();

          // Store the value for the receiver
          ch_->pending_value_ = std::move(value_);

          // Resume the receiver which will get the value
          recv_h.resume();
          // The sender doesn't need to wait since operation is complete
          // We can resume the sender immediately to continue execution
          h.resume();
          return;
        }

        // No receiver available, wait in send queue
        ch_->send_queue_.push(std::make_pair(h, std::move(value_)));
      }

      void await_resume() const noexcept {
        // Send operation completed
      }
    };

    return send_awaitable{this, std::move(value)};
  }

  auto recv() {
    struct recv_awaitable {
      channel* ch_;

      bool await_ready() const noexcept {
        // For simplicity in this implementation, always return false
        // to avoid complex await_ready/await_suspend interaction
        return false;
      }

      void await_suspend(std::coroutine_handle<> h) {
        // Handle buffered value
        if (!ch_->buffer_.empty()) {
          ch_->pending_value_ = std::move(ch_->buffer_.front());
          ch_->buffer_.pop();
          h.resume();  // Resume immediately since value is available
          return;
        }

        // Check if there's a waiting sender to pair with
        if (!ch_->send_queue_.empty()) {
          auto [sender_h, value] = std::move(ch_->send_queue_.front());
          ch_->send_queue_.pop();

          // Store the value for this receiver
          ch_->pending_value_ = std::move(value);

          // Resume the sender to complete its operation
          sender_h.resume();
          // Also resume this receiver to continue execution after co_await
          h.resume();
          return;
        }

        // No sender available, wait in recv queue
        ch_->recv_queue_.push(h);
      }

      T await_resume() {
        // Retrieve and return the received value
        T value = std::move(ch_->pending_value_.value());
        ch_->pending_value_.reset();
        return value;
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
  std::queue<T> buffer_;  // For buffered channels
  std::queue<std::pair<std::coroutine_handle<>, T>> send_queue_;  // Waiting senders with values
  std::queue<std::coroutine_handle<>> recv_queue_;  // Waiting receivers
  std::optional<T> pending_value_;  // Temporary storage for value passing
  bool closed_ = false;
};

// Specialization for unbuffered channel (capacity=0)
template <typename T>
using unbuffered_channel = channel<T>;

}  // namespace coro