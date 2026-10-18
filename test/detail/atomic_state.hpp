#pragma once

#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>

#include "assert_def.h"
#include "coro/detail/atomic.hpp"
#include "coro/detail/waiter_registration.hpp"

namespace atomic_state_test {

enum class phase { idle, ready };

static_assert(sizeof(coro::detail::atomic_st<bool>) == sizeof(bool));
static_assert(sizeof(coro::detail::atomic_st<size_t>) == sizeof(size_t));
static_assert(alignof(coro::detail::atomic_st<size_t>) == alignof(size_t));
static_assert(sizeof(coro::detail::atomic_st<phase>) == sizeof(phase));
static_assert(!std::is_copy_constructible_v<coro::detail::atomic_st<int>>);
static_assert(!std::is_copy_assignable_v<coro::detail::atomic_st<int>>);
static_assert(!std::is_move_constructible_v<coro::detail::atomic_st<int>>);
#ifdef CORO_SINGLE_THREAD
static_assert(std::is_same_v<coro::detail::atomic<size_t>, coro::detail::atomic_st<size_t>>);
#else
static_assert(std::is_same_v<coro::detail::atomic<size_t>, std::atomic<size_t>>);
#endif

template <typename STATE, typename T>
void check_operations(T initial, T desired) {
  STATE empty{};
  ASSERT(empty.load() == T{});
  STATE state{initial};
  const auto& readable = state;
  ASSERT(readable.load(std::memory_order_relaxed) == initial);
  state.store(desired, std::memory_order_release);
  ASSERT(readable.load(std::memory_order_acquire) == desired);
  ASSERT(state.exchange(initial, std::memory_order_acq_rel) == desired);
  T expected = desired;
  ASSERT(!state.compare_exchange_strong(expected, desired, std::memory_order_acq_rel));
  ASSERT(expected == initial && state.load() == initial);
  ASSERT(state.compare_exchange_strong(expected, desired));
  ASSERT(expected == initial && state.load() == desired);
}

template <typename MUTEX>
void check_registration() {
  coro::detail::waiter_registration<MUTEX> registration;
  ASSERT(!registration.notify());
  ASSERT(!registration.suspend());
  registration.reset();
  ASSERT(registration.suspend());
  auto copy = registration;
  ASSERT(copy.notify());
  ASSERT(!copy.notify());
  copy = registration;
  ASSERT(copy.notify());
  ASSERT(registration.notify());
  ASSERT(!registration.notify());
}

inline void run() {
  check_operations<coro::detail::atomic_st<bool>>(false, true);
  check_operations<coro::detail::atomic_st<size_t>>(size_t{0}, std::numeric_limits<size_t>::max());
  check_operations<coro::detail::atomic_st<phase>>(phase::idle, phase::ready);
  check_operations<coro::detail::atomic<bool>>(false, true);
  check_operations<coro::detail::atomic<size_t>>(size_t{0}, std::numeric_limits<size_t>::max());
  check_operations<coro::detail::atomic<phase>>(phase::idle, phase::ready);
  check_registration<coro::dummy_mutex>();
  check_registration<std::mutex>();

  // Explicit multithreaded waiters must still publish across threads in CORO_SINGLE_THREAD builds.
  for (int iteration = 0; iteration < 128; ++iteration) {
    coro::detail::waiter_registration<std::mutex> registration;
    int payload = 0;
    bool notify_resumes = false;
    std::thread notifier([&] {
      payload = 42;
      notify_resumes = registration.notify();
    });
    bool suspended = registration.suspend();
    if (!suspended) {
      ASSERT(payload == 42);
    }
    notifier.join();
    ASSERT(suspended == notify_resumes);
    ASSERT(payload == 42);
  }
}

}  // namespace atomic_state_test
