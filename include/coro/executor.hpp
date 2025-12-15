#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace coro {
struct executor {
  virtual ~executor() = default;
  virtual void dispatch(std::function<void()> fn) = 0;
  virtual void post_delayed_ns(std::function<void()> fn, uint64_t delay_ns) = 0;
  virtual void stop() = 0;
};
}  // namespace coro
