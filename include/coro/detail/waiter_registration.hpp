#pragma once

#include <atomic>
#include <type_traits>
#include <utility>

#include "coro/dummy_mutex.hpp"

namespace coro::detail {

// Registration finishes all lock operations before handing resumption to a notifier.
template <typename MUTEX>
class waiter_registration {
  enum class phase { registering, suspended, notified };
  using phase_state = std::conditional_t<std::is_same_v<MUTEX, dummy_mutex>, phase, std::atomic<phase>>;
  phase_state phase_{phase::registering};

  phase current_phase() const noexcept {
    if constexpr (std::is_same_v<MUTEX, dummy_mutex>) {
      return phase_;
    } else {
      return phase_.load(std::memory_order_relaxed);
    }
  }

  void set_phase(phase value) noexcept {
    if constexpr (std::is_same_v<MUTEX, dummy_mutex>) {
      phase_ = value;
    } else {
      phase_.store(value, std::memory_order_relaxed);
    }
  }

 public:
  waiter_registration() = default;
  waiter_registration(const waiter_registration& other) noexcept : phase_(other.current_phase()) {}
  waiter_registration& operator=(const waiter_registration& other) noexcept {
    set_phase(other.current_phase());
    return *this;
  }

  void reset() noexcept {
    set_phase(phase::registering);
  }

  bool suspend() noexcept {
    if constexpr (std::is_same_v<MUTEX, dummy_mutex>) {
      if (phase_ == phase::notified) {
        return false;
      }
      phase_ = phase::suspended;
      return true;
    } else {
      auto expected = phase::registering;
      return phase_.compare_exchange_strong(expected, phase::suspended, std::memory_order_acq_rel);
    }
  }

  bool notify() noexcept {
    if constexpr (std::is_same_v<MUTEX, dummy_mutex>) {
      return std::exchange(phase_, phase::notified) == phase::suspended;
    } else {
      return phase_.exchange(phase::notified, std::memory_order_acq_rel) == phase::suspended;
    }
  }
};

}  // namespace coro::detail
