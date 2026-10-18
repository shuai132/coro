#pragma once

#include <cstdint>
#include <functional>

namespace coro {
// Scheduling interface; concrete execution contexts define shutdown and lifetime requirements.
struct executor {
  virtual ~executor() = default;
  // May invoke fn inline in the executor's execution context, reentering user code; otherwise submit it.
  virtual void dispatch(std::function<void()> fn) = 0;
  // Submit fn without invoking it inline from this call. Another execution thread may run it before this call returns.
  virtual void post(std::function<void()> fn) = 0;
  // Submit fn for delayed execution; a zero delay still queues it rather than invoking it inline.
  virtual void post_delayed_ns(std::function<void()> fn, uint64_t delay_ns) = 0;
};
}  // namespace coro
