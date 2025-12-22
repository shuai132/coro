#pragma once

#include <mutex>

#include "coro/coro.hpp"
#include "coro/dummy_mutex.hpp"
#include "coro/wait_group.hpp"

namespace coro {

// Coroutine-safe latch (countdown latch)
// A latch is a single-use barrier that allows one or more coroutines to wait
// until a set of operations being performed in other coroutines completes.
//
// Unlike wait_group, a latch cannot be incremented after construction.
// Once the count reaches zero, all waiting coroutines are released and
// any subsequent wait() calls return immediately.
//
// Template parameter MUTEX controls internal thread safety:
//   - std::mutex: Thread-safe for multithreaded use (default)
//   - dummy_mutex: No lock overhead for single-threaded use
template <typename MUTEX = std::mutex>
struct latch_t {
 public:
  // Construct a latch with the given count
  // count must be >= 0
  explicit latch_t(int count) : wg_() {
    if (count > 0) {
      wg_.add(count);
    }
  }

  // Count down the latch by n (default 1)
  // When the count reaches zero, all waiting coroutines are resumed
  void count_down(int n = 1) {
    wg_.add(-n);
  }

  // Decrements the internal counter by 1 and blocks until it reaches 0
  // Usage: co_await latch.arrive_and_wait();
  auto arrive_and_wait() {
    wg_.done();
    return wg_.wait();
  }

  // Wait for the counter to reach zero
  // Usage: co_await latch.wait();
  auto wait() {
    return wg_.wait();
  }

  // Support direct co_await on latch object
  // Usage: co_await latch;
  auto operator co_await() const noexcept {
    return wg_.operator co_await();
  }

  // Try to wait (non-blocking check)
  // Returns true if the latch count is zero
  bool try_wait() const noexcept {
    return wg_.get_count() == 0;
  }

  // Get current counter value (for debugging/testing)
  int get_count() const {
    return wg_.get_count();
  }

 private:
  mutable wait_group_t<MUTEX> wg_;  // Internal wait group for synchronization
};

// Type aliases for convenience
using latch = latch_t<std::mutex>;
using latch_mt = latch_t<std::mutex>;
using latch_st = latch_t<dummy_mutex>;

}  // namespace coro
