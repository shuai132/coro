#pragma once

#include <atomic>
#include <cassert>
#include <mutex>
#include <optional>
#include <queue>

#include "coro/coro.hpp"
#include "coro/dummy_mutex.hpp"

namespace coro {

template <typename T, typename MUTEX = std::mutex>
struct channel {
  explicit channel(size_t capacity = 0) : capacity_(capacity) {}

  ~channel() {
    close();
  }

  struct send_awaitable {
    channel* ch_;
    T value_;
    std::coroutine_handle<> handle_{};
    executor* exec_ = nullptr;
    send_awaitable* next_ = nullptr;

    bool await_ready() const noexcept {
      // Check if channel is closed without suspending
      return ch_->closed_.load(std::memory_order_acquire);
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      std::unique_lock<MUTEX> lock(ch_->mutex_);

      if (ch_->closed_.load(std::memory_order_relaxed)) {
        // If channel is closed, don't suspend - just return false to allow await_resume to handle it
        return false;
      }

      // Check if there's a waiting receiver for direct transfer
      if (ch_->recv_queue_head_ != nullptr) {
        auto* recv_awaiter = ch_->recv_queue_head_;
        ch_->recv_queue_head_ = recv_awaiter->next_;

        // Store the value in the receiver's pending value slot
        recv_awaiter->pending_value_ = std::move(value_);

        // Resume receiver immediately with the value
        auto recv_handle = recv_awaiter->handle_;
        lock.unlock();
        if (recv_awaiter->exec_) {
          recv_awaiter->exec_->dispatch([recv_handle]() {
            recv_handle.resume();
          });
        } else {
          recv_handle.resume();
        }

        // Return false to continue execution without suspension
        return false;
      }

      // Check if buffer has space
      if (ch_->capacity_ > 0 && ch_->buffer_.size() < ch_->capacity_) {
        // Put data in buffer, sender can proceed immediately
        ch_->buffer_.push(std::move(value_));
        return false;  // Don't suspend
      }

      // No receiver available and buffer is full, wait in send queue
      exec_ = h.promise().executor_;
      handle_ = h;
      next_ = ch_->send_queue_head_;
      ch_->send_queue_head_ = this;
      return true;  // Suspend
    }

    bool await_resume() {
      return !ch_->closed_.load(std::memory_order_acquire);
    }
  };

  auto send(T value) {
    return send_awaitable{this, std::move(value)};
  }

  struct recv_awaitable {
    channel* ch_;
    std::coroutine_handle<> handle_;
    executor* exec_ = nullptr;
    std::optional<T> pending_value_ = std::nullopt;
    recv_awaitable* next_ = nullptr;

    bool await_ready() const noexcept {
      // Check if channel is closed and no data available without suspending
      return ch_->closed_.load(std::memory_order_acquire) && ch_->buffer_.empty();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      std::unique_lock<MUTEX> lock(ch_->mutex_);

      if (ch_->closed_.load(std::memory_order_relaxed)) {
        // If channel is closed, don't suspend - just return false to allow await_resume to handle it
        return false;
      }

      // Check if buffer has data
      if (!ch_->buffer_.empty()) {
        // Data available in buffer, don't suspend
        return false;
      }

      // Check if there's a waiting sender for direct transfer
      if (ch_->send_queue_head_ != nullptr) {
        auto* send_awaiter = ch_->send_queue_head_;
        ch_->send_queue_head_ = send_awaiter->next_;
        // Store the value in our local pending value
        pending_value_ = std::move(send_awaiter->value_);

        // Resume sender immediately so it can continue
        auto send_handle = send_awaiter->handle_;
        lock.unlock();
        if (send_awaiter->exec_) {
          send_awaiter->exec_->dispatch([send_handle]() {
            send_handle.resume();
          });
        } else {
          send_handle.resume();
        }

        // Return false to continue execution without suspension
        return false;
      }

      // No data available, wait in recv queue with our own pending value storage
      handle_ = h;
      exec_ = h.promise().executor_;
      this->next_ = ch_->recv_queue_head_;
      ch_->recv_queue_head_ = this;
      return true;  // Suspend
    }

    std::optional<T> await_resume() {
      std::unique_lock<MUTEX> lock(ch_->mutex_);

      bool is_closed = ch_->closed_.load(std::memory_order_relaxed);
      if (is_closed && ch_->buffer_.empty() && !pending_value_.has_value()) {
        return std::nullopt;  // Channel is closed and no data available
      }

      // Check if we have a pending value from direct transfer
      if (pending_value_.has_value()) {
        auto result = std::move(pending_value_.value());
        pending_value_.reset();
        return result;
      }

      // Check buffer
      if (!ch_->buffer_.empty()) {
        auto result = std::move(ch_->buffer_.front());
        ch_->buffer_.pop();

        // After reading from buffer, check if there are waiting senders to move to buffer
        if (ch_->send_queue_head_ != nullptr && ch_->buffer_.size() < ch_->capacity_) {
          auto* sender_awaiter = ch_->send_queue_head_;
          ch_->send_queue_head_ = sender_awaiter->next_;
          ch_->buffer_.push(std::move(sender_awaiter->value_));

          // Resume the sender since its value is now in the buffer
          auto sender_h = sender_awaiter->handle_;
          lock.unlock();
          if (sender_awaiter->exec_) {
            sender_awaiter->exec_->dispatch([sender_h]() {
              sender_h.resume();
            });
          } else {
            sender_h.resume();
          }
        }

        return result;
      }

      // This should not happen in normal flow
      assert(false);
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
    // Collect the head of the linked lists to resume outside the lock
    send_awaitable* senders_to_resume = nullptr;
    recv_awaitable* receivers_to_resume = nullptr;

    {
      std::lock_guard<MUTEX> lock(mutex_);

      bool expected = false;
      if (!closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;  // Already closed
      }

      // Take ownership of the entire linked list of senders
      senders_to_resume = send_queue_head_;
      send_queue_head_ = nullptr;  // Clear the head

      // Take ownership of the entire linked list of receivers
      receivers_to_resume = recv_queue_head_;
      recv_queue_head_ = nullptr;  // Clear the head
    }

    // Wake up all waiting senders outside the lock
    auto* send_current = senders_to_resume;
    while (send_current != nullptr) {
      auto h = send_current->handle_;
      auto exec = send_current->exec_;
      auto* next = send_current->next_;
      send_current->next_ = nullptr;  // Clear the link for safety
      if (exec) {
        exec->dispatch([h]() {
          h.resume();
        });
      } else {
        h.resume();
      }
      send_current = next;
    }

    // Wake up all waiting receivers outside the lock
    auto* recv_current = receivers_to_resume;
    while (recv_current != nullptr) {
      auto h = recv_current->handle_;
      auto exec = recv_current->exec_;
      auto* next = recv_current->next_;
      recv_current->next_ = nullptr;  // Clear the link for safety
      if (exec) {
        exec->dispatch([h]() {
          h.resume();
        });
      } else {
        h.resume();
      }
      recv_current = next;
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
  MUTEX mutex_;
  std::queue<T> buffer_;                       // For buffered channels
  send_awaitable* send_queue_head_ = nullptr;  // Head of waiting senders linked list
  recv_awaitable* recv_queue_head_ = nullptr;  // Head of waiting receivers linked list
  std::atomic<bool> closed_{false};
};

template <typename T>
using channel_mt = channel<T, std::mutex>;

template <typename T>
using channel_st = channel<T, dummy_mutex>;

// Aliases for convenience
template <typename T, typename M>
using unbuffered_channel = channel<T, M>;

template <typename T>
using unbuffered_channel_mt = unbuffered_channel<T, std::mutex>;

template <typename T>
using unbuffered_channel_st = unbuffered_channel<T, dummy_mutex>;

}  // namespace coro
