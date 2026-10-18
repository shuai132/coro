#pragma once

#include <cstddef>
#include <utility>

namespace coro::detail {

// Internal single-thread ownership only; all copies must be used and destroyed on one thread.
// The count and value share one allocation. No weak ownership, aliasing or custom deleters.
template <typename T>
class shared_ptr_st {
  struct storage {
    std::size_t count_ = 1;
    T value_;

    template <typename... ARGS>
    explicit storage(ARGS&&... args) : value_(std::forward<ARGS>(args)...) {}
  };

  storage* storage_ = nullptr;
  // Keep the value address explicit: GCC 14 misdiagnoses atomic member access when
  // its optimizer propagates an empty control pointer through &storage_->value_.
  T* ptr_ = nullptr;

  explicit shared_ptr_st(storage* value) noexcept : storage_(value), ptr_(&value->value_) {}

 public:
  shared_ptr_st() noexcept = default;

  template <typename... ARGS>
  static shared_ptr_st make(ARGS&&... args) {
    return shared_ptr_st(new storage(std::forward<ARGS>(args)...));
  }

  shared_ptr_st(const shared_ptr_st& other) noexcept : storage_(other.storage_), ptr_(other.ptr_) {
    if (storage_) {
      ++storage_->count_;
    }
  }

  shared_ptr_st(shared_ptr_st&& other) noexcept : storage_(std::exchange(other.storage_, nullptr)), ptr_(std::exchange(other.ptr_, nullptr)) {}

  shared_ptr_st& operator=(shared_ptr_st other) noexcept {
    std::swap(storage_, other.storage_);
    std::swap(ptr_, other.ptr_);
    return *this;
  }

  ~shared_ptr_st() {
    if (storage_ && --storage_->count_ == 0) {
      delete storage_;
    }
  }

  T* operator->() const noexcept {
    return ptr_;
  }

  T* get() const noexcept {
    return ptr_;
  }

  explicit operator bool() const noexcept {
    return ptr_ != nullptr;
  }

  std::size_t use_count() const noexcept {
    return storage_ ? storage_->count_ : 0;
  }
};

}  // namespace coro::detail
