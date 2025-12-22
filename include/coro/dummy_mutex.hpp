#pragma once

namespace coro {

// Dummy mutex for single-threaded scenarios (no synchronization overhead)
// Used as a template parameter for lock-free single-threaded containers
struct dummy_mutex {
  constexpr void lock() noexcept {}
  constexpr void unlock() noexcept {}
};

}  // namespace coro
