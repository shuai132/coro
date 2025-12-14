#pragma once

#include <cstdint>
#include <functional>

namespace coro {
struct executor {
  virtual ~executor() = default;
  virtual void dispatch(std::function<void()> fn) = 0;
  virtual void post_delayed(std::function<void()> fn, uint32_t delay) = 0;
  virtual void stop() = 0;
};
}  // namespace coro
