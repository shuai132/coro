#include "coro/executor.hpp"

inline void check_coro_leak() {
#ifdef CORO_DEBUG_PROMISE_LEAK
  std::lock_guard<std::mutex> lock(debug_coro_promise::debug_coro_leak_mutex);
  LOG("debug: debug_coro_leak.size: %zu", debug_coro_promise::debug_coro_leak.size());
  ASSERT(debug_coro_promise::debug_coro_leak.empty());
#endif
}

template <typename EXECUTOR>
inline void delay_stop(EXECUTOR& executor, int wait_ms = 1000) {
  executor.post_delayed_ns(
      [&executor] {
        executor.stop();
      },
      wait_ms > 0 ? static_cast<uint64_t>(wait_ms) * 1000000 : 0);
}
