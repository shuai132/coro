#pragma once

#include <chrono>
#include <cstdint>

#include "coro.hpp"

namespace coro {

using namespace std::chrono_literals;

template <typename Rep, typename Period>
inline callback_awaiter<void> sleep(std::chrono::duration<Rep, Period> duration) {
  auto ns = duration > duration.zero() ? std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() : 0;
  return callback_awaiter<void>([ns](auto executor, auto callback) {
    executor->post_delayed_ns(std::move(callback), static_cast<uint64_t>(ns));
  });
}

inline callback_awaiter<void> delay(uint64_t ms) {
  using nanoseconds = std::chrono::nanoseconds;
  constexpr auto max_ns = static_cast<uint64_t>(nanoseconds::max().count());
  auto ns = ms > max_ns / 1000000 ? max_ns : ms * 1000000;
  return sleep(nanoseconds(static_cast<nanoseconds::rep>(ns)));
}

template <typename Rep, typename Period>
inline auto operator co_await(std::chrono::duration<Rep, Period> duration) {
  return sleep(duration);
}

}  // namespace coro
