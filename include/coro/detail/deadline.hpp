#pragma once

#include <chrono>
#include <cstdint>

namespace coro::detail {

inline std::chrono::steady_clock::time_point delayed_deadline(uint64_t delay_ns) {
  using clock = std::chrono::steady_clock;
  using nanoseconds = std::chrono::nanoseconds;
  if (delay_ns > static_cast<uint64_t>(nanoseconds::max().count())) {
    return clock::time_point::max();
  }
  auto delay = std::chrono::duration_cast<clock::duration>(nanoseconds(static_cast<nanoseconds::rep>(delay_ns)));
  auto now = clock::now();
  if (now > clock::time_point::max() - delay) {
    return clock::time_point::max();
  }
  return now + delay;
}

}  // namespace coro::detail
