#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace coro {
struct executor {
  virtual ~executor() = default;
  virtual void dispatch(std::function<void()> fn) = 0;
  virtual void post(std::function<void()> fn) = 0;
  // Queue a suspended operation's continuation; custom executors may preserve draining work after stop().
  virtual void post_continuation(std::function<void()> fn) {
    post(std::move(fn));
  }
  virtual void post_delayed_ns(std::function<void()> fn, uint64_t delay_ns) = 0;
};
}  // namespace coro
