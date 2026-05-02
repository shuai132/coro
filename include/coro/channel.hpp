#pragma once

#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

#include "coro/coro.hpp"
#include "coro/detail/resume.hpp"
#include "coro/dummy_mutex.hpp"

namespace coro {

template <typename T, typename MUTEX = std::mutex>
struct channel {
  struct send_awaitable;
  struct broadcast_awaitable;
  struct recv_awaitable;

 private:
  struct state {
    explicit state(size_t capacity) : capacity_(capacity) {}

    size_t capacity_;
    mutable MUTEX mutex_;
    std::queue<T> buffer_;                       // For buffered channels
    send_awaitable* send_queue_head_ = nullptr;  // Head of waiting senders linked list
    send_awaitable* send_queue_tail_ = nullptr;  // Tail of waiting senders linked list
    recv_awaitable* recv_queue_head_ = nullptr;  // Head of waiting receivers linked list
    recv_awaitable* recv_queue_tail_ = nullptr;  // Tail of waiting receivers linked list
    std::atomic<bool> closed_{false};
  };

 public:
  explicit channel(size_t capacity = 0) : state_(std::make_shared<state>(capacity)) {}

  ~channel() {
    close();
  }

  channel(const channel&) = delete;
  channel(channel&&) = delete;
  channel& operator=(const channel&) = delete;
  channel& operator=(channel&&) = delete;

  struct send_awaitable {
    std::shared_ptr<state> state_;
    T value_;
    std::coroutine_handle<> handle_{};
    executor* exec_ = nullptr;
    send_awaitable* next_ = nullptr;
    bool delivered_ = false;

    send_awaitable(std::shared_ptr<state> state, T value) : state_(std::move(state)), value_(std::move(value)) {}

    bool await_ready() const noexcept {
      // Check if channel is closed without suspending
      return state_->closed_.load(std::memory_order_acquire);
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      std::unique_lock<MUTEX> lock(state_->mutex_);

      if (state_->closed_.load(std::memory_order_relaxed)) {
        // If channel is closed, don't suspend - just return false to allow await_resume to handle it
        return false;
      }

      // Check if there's a waiting receiver for direct transfer
      if (state_->recv_queue_head_ != nullptr) {
        auto* recv_awaiter = state_->recv_queue_head_;
        state_->recv_queue_head_ = recv_awaiter->next_;
        if (state_->recv_queue_head_ == nullptr) {
          state_->recv_queue_tail_ = nullptr;
        }
        recv_awaiter->next_ = nullptr;

        // Store the value in the receiver's pending value slot
        recv_awaiter->pending_value_.emplace(std::move(value_));
        delivered_ = true;

        // Resume receiver immediately with the value
        auto recv_handle = recv_awaiter->handle_;
        auto recv_exec = recv_awaiter->exec_;
        lock.unlock();
        detail::resume_via(recv_exec, recv_handle);

        // Return false to continue execution without suspension
        return false;
      }

      // Check if buffer has space
      if (state_->capacity_ > 0 && state_->buffer_.size() < state_->capacity_) {
        // Put data in buffer, sender can proceed immediately
        state_->buffer_.push(std::move(value_));
        delivered_ = true;
        return false;  // Don't suspend
      }

      // No receiver available and buffer is full, wait in send queue
      exec_ = h.promise().executor_;
      handle_ = h;
      next_ = nullptr;
      if (state_->send_queue_tail_) {
        state_->send_queue_tail_->next_ = this;
      } else {
        state_->send_queue_head_ = this;
      }
      state_->send_queue_tail_ = this;
      return true;  // Suspend
    }

    bool await_resume() {
      return delivered_;
    }
  };

  auto send(T value) {
    return send_awaitable{state_, std::move(value)};
  }

  // Broadcast awaitable: sends the value to ALL waiting receivers
  struct broadcast_awaitable {
    std::shared_ptr<state> state_;
    T value_;
    std::coroutine_handle<> handle_{};
    executor* exec_ = nullptr;
    size_t receivers_notified_ = 0;

    broadcast_awaitable(std::shared_ptr<state> state, T value) : state_(std::move(state)), value_(std::move(value)) {}

    bool await_ready() const noexcept {
      return state_->closed_.load(std::memory_order_acquire);
    }

    template <typename Promise>
    bool await_suspend([[maybe_unused]] std::coroutine_handle<Promise> h) {
      recv_awaitable* receivers_to_resume = nullptr;

      {
        std::lock_guard<MUTEX> lock(state_->mutex_);

        if (state_->closed_.load(std::memory_order_relaxed)) {
          return false;
        }

        // Broadcast to ALL waiting receivers (not just one)
        receivers_to_resume = state_->recv_queue_head_;
        state_->recv_queue_head_ = nullptr;
        state_->recv_queue_tail_ = nullptr;
      }

      auto* recv_awaiter = receivers_to_resume;
      while (recv_awaiter != nullptr) {
        // Copy the value to each receiver's pending value
        recv_awaiter->pending_value_.emplace(value_);
        receivers_notified_++;

        auto recv_handle = recv_awaiter->handle_;
        auto recv_exec = recv_awaiter->exec_;
        auto* next = recv_awaiter->next_;
        recv_awaiter->next_ = nullptr;

        // Resume the receiver asynchronously so a broadcast cannot reenter a waiter inline.
        detail::post_resume_via(recv_exec, recv_handle);
        recv_awaiter = next;
      }

      // No receivers waiting - for broadcast, we typically don't buffer
      // and just complete immediately. If you want buffering behavior,
      // you can modify this logic.
      return false;
    }

    size_t await_resume() {
      return receivers_notified_;
    }
  };

  // Broadcast: sends value to ALL waiting receivers
  // Returns the number of receivers that received the broadcast
  auto broadcast(T value) {
    return broadcast_awaitable{state_, std::move(value)};
  }

  struct recv_awaitable {
    std::shared_ptr<state> state_;
    std::coroutine_handle<> handle_;
    executor* exec_ = nullptr;
    std::optional<T> pending_value_ = std::nullopt;
    recv_awaitable* next_ = nullptr;

    explicit recv_awaitable(std::shared_ptr<state> state) : state_(std::move(state)) {}

    bool await_ready() const noexcept {
      // Check if channel is closed and no data available without suspending
      std::lock_guard<MUTEX> lock(state_->mutex_);
      return state_->closed_.load(std::memory_order_acquire) && state_->buffer_.empty();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      std::unique_lock<MUTEX> lock(state_->mutex_);

      if (state_->closed_.load(std::memory_order_relaxed)) {
        // If channel is closed, don't suspend - just return false to allow await_resume to handle it
        return false;
      }

      // Check if buffer has data
      if (!state_->buffer_.empty()) {
        // Data available in buffer, don't suspend
        return false;
      }

      // Check if there's a waiting sender for direct transfer
      if (state_->send_queue_head_ != nullptr) {
        auto* send_awaiter = state_->send_queue_head_;
        state_->send_queue_head_ = send_awaiter->next_;
        if (state_->send_queue_head_ == nullptr) {
          state_->send_queue_tail_ = nullptr;
        }
        send_awaiter->next_ = nullptr;
        // Store the value in our local pending value
        pending_value_.emplace(std::move(send_awaiter->value_));
        send_awaiter->delivered_ = true;

        // Resume sender immediately so it can continue
        auto send_handle = send_awaiter->handle_;
        auto send_exec = send_awaiter->exec_;
        lock.unlock();
        detail::resume_via(send_exec, send_handle);

        // Return false to continue execution without suspension
        return false;
      }

      // No data available, wait in recv queue with our own pending value storage
      handle_ = h;
      exec_ = h.promise().executor_;
      this->next_ = nullptr;
      if (state_->recv_queue_tail_) {
        state_->recv_queue_tail_->next_ = this;
      } else {
        state_->recv_queue_head_ = this;
      }
      state_->recv_queue_tail_ = this;
      return true;  // Suspend
    }

    std::optional<T> await_resume() {
      std::unique_lock<MUTEX> lock(state_->mutex_);

      // Check if we have a pending value from direct transfer
      if (pending_value_.has_value()) {
        auto result = std::move(pending_value_.value());
        pending_value_.reset();
        return result;
      }

      // Check buffer
      if (!state_->buffer_.empty()) {
        auto result = std::move(state_->buffer_.front());
        state_->buffer_.pop();

        // After reading from buffer, check if there are waiting senders to move to buffer
        if (state_->send_queue_head_ != nullptr && state_->buffer_.size() < state_->capacity_) {
          auto* sender_awaiter = state_->send_queue_head_;
          state_->send_queue_head_ = sender_awaiter->next_;
          if (state_->send_queue_head_ == nullptr) {
            state_->send_queue_tail_ = nullptr;
          }
          sender_awaiter->next_ = nullptr;
          state_->buffer_.push(std::move(sender_awaiter->value_));
          sender_awaiter->delivered_ = true;

          // Resume the sender since its value is now in the buffer
          auto sender_h = sender_awaiter->handle_;
          auto sender_exec = sender_awaiter->exec_;
          lock.unlock();
          detail::resume_via(sender_exec, sender_h);
        }

        return result;
      }

      bool is_closed = state_->closed_.load(std::memory_order_relaxed);
      if (is_closed) {
        return std::nullopt;  // Channel is closed and no data available
      }

      // This should not happen in normal flow
      assert(false);
      return std::nullopt;
    }
  };

  auto recv() {
    return recv_awaitable{state_};
  }

  bool is_closed() const {
    return state_->closed_.load(std::memory_order_acquire);
  }

  void close() {
    auto state = state_;

    // Collect the head of the linked lists to resume outside the lock
    send_awaitable* senders_to_resume = nullptr;
    recv_awaitable* receivers_to_resume = nullptr;

    {
      std::lock_guard<MUTEX> lock(state->mutex_);

      bool expected = false;
      if (!state->closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;  // Already closed
      }

      // Take ownership of the entire linked list of senders
      senders_to_resume = state->send_queue_head_;
      state->send_queue_head_ = nullptr;  // Clear the head
      state->send_queue_tail_ = nullptr;  // Clear the tail

      // Take ownership of the entire linked list of receivers
      receivers_to_resume = state->recv_queue_head_;
      state->recv_queue_head_ = nullptr;  // Clear the head
      state->recv_queue_tail_ = nullptr;  // Clear the tail
    }

    // Wake up all waiting senders outside the lock
    auto* send_current = senders_to_resume;
    while (send_current != nullptr) {
      auto h = send_current->handle_;
      auto exec = send_current->exec_;
      auto* next = send_current->next_;
      send_current->next_ = nullptr;  // Clear the link for safety
      detail::resume_via(exec, h);
      send_current = next;
    }

    // Wake up all waiting receivers outside the lock
    auto* recv_current = receivers_to_resume;
    while (recv_current != nullptr) {
      auto h = recv_current->handle_;
      auto exec = recv_current->exec_;
      auto* next = recv_current->next_;
      recv_current->next_ = nullptr;  // Clear the link for safety
      detail::resume_via(exec, h);
      recv_current = next;
    }
  }

  bool empty() const {
    std::lock_guard<MUTEX> lock(state_->mutex_);
    return state_->buffer_.empty();
  }

  bool full() const {
    std::lock_guard<MUTEX> lock(state_->mutex_);
    return state_->capacity_ > 0 && state_->buffer_.size() >= state_->capacity_;
  }

  size_t size() const {
    std::lock_guard<MUTEX> lock(state_->mutex_);
    return state_->buffer_.size();
  }

  size_t capacity() const {
    return state_->capacity_;
  }

 private:
  std::shared_ptr<state> state_;
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
