#pragma once

#include <atomic>
#include <type_traits>

#include "coro/detail/atomic_st.hpp"
#include "coro/dummy_mutex.hpp"

namespace coro::detail {

// Registration finishes all lock operations before handing resumption to a notifier.
template <typename MUTEX>
class waiter_registration {
  enum class phase { registering, suspended, notified };
  using phase_state = std::conditional_t<std::is_same_v<MUTEX, dummy_mutex>, atomic_st<phase>, std::atomic<phase>>;
  phase_state phase_{phase::registering};

 public:
  waiter_registration() = default;
  waiter_registration(const waiter_registration& other) noexcept : phase_(other.phase_.load(std::memory_order_relaxed)) {}
  waiter_registration& operator=(const waiter_registration& other) noexcept {
    phase_.store(other.phase_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
  }

  void reset() noexcept {
    phase_.store(phase::registering, std::memory_order_relaxed);
  }

  bool suspend() noexcept {
    auto expected = phase::registering;
    return phase_.compare_exchange_strong(expected, phase::suspended, std::memory_order_acq_rel);
  }

  bool notify() noexcept {
    return phase_.exchange(phase::notified, std::memory_order_acq_rel) == phase::suspended;
  }
};

}  // namespace coro::detail
