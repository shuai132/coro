#pragma once

#include <atomic>
#include <type_traits>
#include <utility>

namespace coro::detail {

// Atomic-style access to single-threaded integer and enum state. Memory orders provide no synchronization here.
template <typename T>
class atomic_st {
  static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
  T value_{};

 public:
  constexpr atomic_st() noexcept = default;
  constexpr atomic_st(T value) noexcept : value_(value) {}
  atomic_st(const atomic_st&) = delete;
  atomic_st& operator=(const atomic_st&) = delete;

  constexpr T load(std::memory_order = std::memory_order_seq_cst) const noexcept {
    return value_;
  }

  constexpr void store(T value, std::memory_order = std::memory_order_seq_cst) noexcept {
    value_ = value;
  }

  constexpr T exchange(T value, std::memory_order = std::memory_order_seq_cst) noexcept {
    return std::exchange(value_, value);
  }

  constexpr bool compare_exchange_strong(T& expected, T desired, std::memory_order = std::memory_order_seq_cst) noexcept {
    if (value_ == expected) {
      value_ = desired;
      return true;
    }
    expected = value_;
    return false;
  }
};

}  // namespace coro::detail
