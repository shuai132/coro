#pragma once

#include <mutex>

#include "coro/coro.hpp"
#include "coro/dummy_mutex.hpp"
#include "coro/wait_group.hpp"

namespace coro {

// Coroutine-safe event synchronization primitive
// The event allows one or more coroutines to wait for an event to be set before proceeding.
//
// When the event is set, all waiting coroutines are resumed on the thread that sets the event.
// If the event is already set when a coroutine waits, it will simply continue executing
// with no suspend or wait time incurred.
//
// Once set, the event stays set until explicitly cleared with clear().
//
// Template parameter MUTEX controls internal thread safety:
//   - std::mutex: Thread-safe for multithreaded use (default)
//   - dummy_mutex: No lock overhead for single-threaded use
template <typename MUTEX = std::mutex>
struct event_t {
 public:
  // Construct an event in the unset state
  event_t() : wg_() {
    wg_.add(1);  // Start with counter = 1 (unset state)
  }

  event_t(const event_t&) = delete;
  event_t(event_t&&) = delete;
  event_t& operator=(const event_t&) = delete;
  event_t& operator=(event_t&&) = delete;

  // Set the event and wake up all waiting coroutines
  // All current and future waiters will proceed until clear() is called
  // Usage: evt.set();
  void set() {
    wg_.done();  // Counter becomes 0, all waiters are resumed
  }

  // Clear the event
  // Future waits will block until set() is called again
  // Usage: evt.clear();
  void clear() {
    wg_.add(1);  // Counter becomes 1 again (unset state)
  }

  // Wait for the event to be set
  // If already set, returns immediately without suspending
  // Usage: co_await evt.wait();
  auto wait() const {
    return wg_.wait();
  }

  // Support direct co_await on event object
  // Usage: co_await evt;
  auto operator co_await() const noexcept {
    return wg_.operator co_await();
  }

  // Check if the event is set (non-blocking)
  // Returns true if the event is set
  bool is_set() const noexcept {
    return wg_.get_count() == 0;
  }

 private:
  mutable wait_group_t<MUTEX> wg_;  // Internal wait group for synchronization
};

// Type aliases for convenience
using event = event_t<std::mutex>;
using event_mt = event_t<std::mutex>;
using event_st = event_t<dummy_mutex>;

}  // namespace coro
