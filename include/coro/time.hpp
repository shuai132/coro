#pragma once

#include <chrono>
#include <cstdint>

#include "coro/coro.hpp"

namespace coro {

using namespace std::chrono_literals;

template <typename Rep, typename Period>
inline callback_awaiter<void> sleep(std::chrono::duration<Rep, Period> duration) {
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
  return callback_awaiter<void>([ns](auto executor, auto callback) {
    executor->post_delayed(std::move(callback), static_cast<uint64_t>(ns));
  });
}

inline callback_awaiter<void> delay(uint64_t ms) {
  return sleep(std::chrono::milliseconds(ms));
}

}  // namespace coro
