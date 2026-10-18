#pragma once

#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <type_traits>
#include <utility>

#include "coro/coro.hpp"
#include "coro/detail/resume.hpp"
#include "coro/detail/shared_ptr_st.hpp"
#include "coro/detail/thread_support.hpp"
#include "coro/dummy_mutex.hpp"

namespace coro {

#if CORO_DETAIL_HAS_STD_MUTEX
template <typename T, typename MUTEX = std::mutex>
#else
template <typename T, typename MUTEX>
#endif
struct channel {
  struct send_awaitable;
  struct broadcast_awaitable;
  struct recv_awaitable;

 private:
  struct state {
    explicit state(size_t capacity) : capacity_(capacity) {
      if (capacity_ > 0) {
        buffer_.emplace();
      }
    }

    bool buffer_empty() const noexcept {
      return !buffer_ || buffer_->empty();
    }

    size_t capacity_;
    mutable MUTEX mutex_;
    std::optional<std::queue<T>> buffer_;        // Only buffered channels need message storage.
    send_awaitable* send_queue_head_ = nullptr;  // Head of waiting senders linked list
    send_awaitable* send_queue_tail_ = nullptr;  // Tail of waiting senders linked list
    recv_awaitable* recv_queue_head_ = nullptr;  // Head of waiting receivers linked list
    recv_awaitable* recv_queue_tail_ = nullptr;  // Tail of waiting receivers linked list
    std::atomic<bool> closed_{false};
  };

  using state_ptr = std::conditional_t<std::is_same_v<MUTEX, dummy_mutex>, detail::shared_ptr_st<state>, std::shared_ptr<state>>;

  static state_ptr make_state(size_t capacity) {
    if constexpr (std::is_same_v<MUTEX, dummy_mutex>) {
      return state_ptr::make(capacity);
    } else {
      return std::make_shared<state>(capacity);
    }
  }

 public:
  explicit channel(size_t capacity = 0) : state_(make_state(capacity)) {}

  ~channel() {
    close();
  }

  channel(const channel&) = delete;
  channel(channel&&) = delete;
  channel& operator=(const channel&) = delete;
  channel& operator=(channel&&) = delete;

  struct send_awaitable {
    state_ptr state_;
    T value_;
    std::coroutine_handle<> handle_{};
    executor* exec_ = nullptr;
    send_awaitable* next_ = nullptr;
    bool delivered_ = false;

    send_awaitable(state_ptr state, T value) : state_(std::move(state)), value_(std::move(value)) {}

    bool await_ready() noexcept {
      delivered_ = false;
      // Check if channel is closed without suspending
      return state_->closed_.load(std::memory_order_acquire);
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      // A peer can complete this coroutine during unlock; retain the mutex-owning state until registration returns.
      auto state = state_;
      std::unique_lock<MUTEX> lock(state->mutex_);

      if (state->closed_.load(std::memory_order_relaxed)) {
        // If channel is closed, don't suspend - just return false to allow await_resume to handle it
        return false;
      }

      // Check if there's a waiting receiver for direct transfer
      if (state->recv_queue_head_ != nullptr) {
        auto* recv_awaiter = state->recv_queue_head_;
        // Keep the waiter queued if constructing its result throws.
        recv_awaiter->pending_value_.emplace(std::move(value_));
        state->recv_queue_head_ = recv_awaiter->next_;
        if (state->recv_queue_head_ == nullptr) {
          state->recv_queue_tail_ = nullptr;
        }
        recv_awaiter->next_ = nullptr;

        delivered_ = true;

        // Queue bound receivers so channel pipelines cannot recurse through dispatch.
        auto recv_handle = recv_awaiter->handle_;
        auto recv_exec = recv_awaiter->exec_;
        lock.unlock();
        detail::post_resume_via(recv_exec, recv_handle);

        // Return false to continue execution without suspension
        return false;
      }

      // A failed receive refill may leave space while older senders are still queued.
      if (state->capacity_ > 0 && state->send_queue_head_ == nullptr && state->buffer_->size() < state->capacity_) {
        // Put data in buffer, sender can proceed immediately
        state->buffer_->push(std::move(value_));
        delivered_ = true;
        return false;  // Don't suspend
      }

      // No receiver available and buffer is full, wait in send queue
      exec_ = h.promise().executor_;
      handle_ = h;
      next_ = nullptr;
      if (state->send_queue_tail_) {
        state->send_queue_tail_->next_ = this;
      } else {
        state->send_queue_head_ = this;
      }
      state->send_queue_tail_ = this;
      return true;  // Suspend
    }

    bool await_resume() noexcept {
      return delivered_;
    }
  };

  auto send(T value) {
    return send_awaitable{state_, std::move(value)};
  }

  // Broadcast awaitable: sends the value to ALL waiting receivers
  struct broadcast_awaitable {
    state_ptr state_;
    T value_;
    std::coroutine_handle<> handle_{};
    executor* exec_ = nullptr;
    size_t receivers_notified_ = 0;

    broadcast_awaitable(state_ptr state, T value) : state_(std::move(state)), value_(std::move(value)) {}

    bool await_ready() noexcept {
      receivers_notified_ = 0;
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

        // Prepare every result before removing any waiter from the queue.
#ifndef CORO_DISABLE_EXCEPTION
        try {
#endif
          for (auto* receiver = state_->recv_queue_head_; receiver; receiver = receiver->next_) {
            receiver->pending_value_.emplace(value_);
            ++receivers_notified_;
          }
#ifndef CORO_DISABLE_EXCEPTION
        } catch (...) {
          for (auto* receiver = state_->recv_queue_head_; receiver; receiver = receiver->next_) {
            receiver->pending_value_.reset();
          }
          receivers_notified_ = 0;
          throw;
        }
#endif
        receivers_to_resume = state_->recv_queue_head_;
        state_->recv_queue_head_ = nullptr;
        state_->recv_queue_tail_ = nullptr;
      }

      auto* recv_awaiter = receivers_to_resume;
      while (recv_awaiter != nullptr) {
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

    size_t await_resume() noexcept {
      return receivers_notified_;
    }
  };

  // Broadcast: sends value to ALL waiting receivers
  // Returns the number of receivers that received the broadcast
  auto broadcast(T value) {
    return broadcast_awaitable{state_, std::move(value)};
  }

  struct recv_awaitable {
    state_ptr state_;
    std::coroutine_handle<> handle_;
    executor* exec_ = nullptr;
    std::optional<T> pending_value_ = std::nullopt;
    recv_awaitable* next_ = nullptr;

    explicit recv_awaitable(state_ptr state) noexcept : state_(std::move(state)) {}

    bool await_ready() noexcept(noexcept(std::declval<MUTEX&>().lock())) {
      // A previous await_suspend may have reserved a value before a buffer refill failed.
      pending_value_.reset();
      if (!state_->closed_.load(std::memory_order_acquire)) {
        return false;
      }
      // Check if channel is closed and no data available without suspending
      std::lock_guard<MUTEX> lock(state_->mutex_);
      return state_->buffer_empty();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      // A peer can complete this coroutine during unlock; retain the mutex-owning state until registration returns.
      auto state = state_;
      std::unique_lock<MUTEX> lock(state->mutex_);

      // Check buffer
      if (!state->buffer_empty()) {
        pending_value_.emplace(std::move(state->buffer_->front()));
        state->buffer_->pop();

        // After reading from buffer, check if there are waiting senders to move to buffer
        if (state->send_queue_head_ != nullptr && state->buffer_->size() < state->capacity_) {
          auto* sender_awaiter = state->send_queue_head_;
          state->buffer_->push(std::move(sender_awaiter->value_));
          state->send_queue_head_ = sender_awaiter->next_;
          if (state->send_queue_head_ == nullptr) {
            state->send_queue_tail_ = nullptr;
          }
          sender_awaiter->next_ = nullptr;
          sender_awaiter->delivered_ = true;

          // Resume the sender since its value is now in the buffer
          auto sender_h = sender_awaiter->handle_;
          auto sender_exec = sender_awaiter->exec_;
          lock.unlock();
          detail::post_resume_via(sender_exec, sender_h);
        }

        return false;
      }

      if (state->closed_.load(std::memory_order_relaxed)) {
        // If channel is closed, don't suspend - just return false to allow await_resume to handle it
        return false;
      }

      // Check if there's a waiting sender for direct transfer
      if (state->send_queue_head_ != nullptr) {
        auto* send_awaiter = state->send_queue_head_;
        pending_value_.emplace(std::move(send_awaiter->value_));
        state->send_queue_head_ = send_awaiter->next_;
        if (state->send_queue_head_ == nullptr) {
          state->send_queue_tail_ = nullptr;
        }
        send_awaiter->next_ = nullptr;
        send_awaiter->delivered_ = true;

        // Queue bound senders so channel pipelines cannot recurse through dispatch.
        auto send_handle = send_awaiter->handle_;
        auto send_exec = send_awaiter->exec_;
        lock.unlock();
        detail::post_resume_via(send_exec, send_handle);

        // Return false to continue execution without suspension
        return false;
      }

      // No data available, wait in recv queue with our own pending value storage
      handle_ = h;
      exec_ = h.promise().executor_;
      this->next_ = nullptr;
      if (state->recv_queue_tail_) {
        state->recv_queue_tail_->next_ = this;
      } else {
        state->recv_queue_head_ = this;
      }
      state->recv_queue_tail_ = this;
      return true;  // Suspend
    }

    std::optional<T> await_resume() {
      // A delivered value belongs exclusively to this receiver, which is no longer queued.
      // User value operations may reenter the channel, so do not hold its mutex here.
      if (pending_value_.has_value()) {
        struct result_cleanup {
          std::optional<T>& value_;
          ~result_cleanup() {
            value_.reset();
          }
        } cleanup{pending_value_};
        // A failed extraction consumes this delivery too; never expose it on a later await.
        return std::move(pending_value_);
      }

      bool is_closed = state_->closed_.load(std::memory_order_acquire);
      if (is_closed) {
        return std::nullopt;  // Channel is closed and no data available
      }

      // This should not happen in normal flow
      assert(false);
      return std::nullopt;
    }
  };

  auto recv() noexcept {
    return recv_awaitable{state_};
  }

  bool is_closed() const noexcept {
    return state_->closed_.load(std::memory_order_acquire);
  }

  void close() {
    auto state = state_;

    // Collect the head of the linked lists to resume outside the lock
    send_awaitable* senders_to_resume = nullptr;
    recv_awaitable* receivers_to_resume = nullptr;

    {
      std::lock_guard<MUTEX> lock(state->mutex_);

      if (state->closed_.load(std::memory_order_relaxed)) {
        return;  // Already closed
      }
      // The mutex serializes closers; avoid an atomic read-modify-write dependency on small MCUs.
      state->closed_.store(true, std::memory_order_release);

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
      detail::post_resume_via(exec, h);
      send_current = next;
    }

    // Wake up all waiting receivers outside the lock
    auto* recv_current = receivers_to_resume;
    while (recv_current != nullptr) {
      auto h = recv_current->handle_;
      auto exec = recv_current->exec_;
      auto* next = recv_current->next_;
      recv_current->next_ = nullptr;  // Clear the link for safety
      detail::post_resume_via(exec, h);
      recv_current = next;
    }
  }

  bool empty() const noexcept(noexcept(std::declval<MUTEX&>().lock())) {
    std::lock_guard<MUTEX> lock(state_->mutex_);
    return state_->buffer_empty();
  }

  bool full() const noexcept(noexcept(std::declval<MUTEX&>().lock())) {
    std::lock_guard<MUTEX> lock(state_->mutex_);
    return state_->capacity_ > 0 && state_->buffer_->size() >= state_->capacity_;
  }

  size_t size() const noexcept(noexcept(std::declval<MUTEX&>().lock())) {
    std::lock_guard<MUTEX> lock(state_->mutex_);
    return state_->buffer_ ? state_->buffer_->size() : 0;
  }

  size_t capacity() const noexcept {
    return state_->capacity_;
  }

 private:
  state_ptr state_;
};

#if CORO_DETAIL_HAS_STD_MUTEX
template <typename T>
using channel_mt = channel<T, std::mutex>;
#endif

template <typename T>
using channel_st = channel<T, dummy_mutex>;

// Aliases for convenience
template <typename T, typename M>
using unbuffered_channel = channel<T, M>;

#if CORO_DETAIL_HAS_STD_MUTEX
template <typename T>
using unbuffered_channel_mt = unbuffered_channel<T, std::mutex>;
#endif

template <typename T>
using unbuffered_channel_st = unbuffered_channel<T, dummy_mutex>;

}  // namespace coro
