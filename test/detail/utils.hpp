#include <thread>

#include "coro/executor.hpp"

inline std::thread debug_and_stop(coro::executor& executor, int wait_ms = 1000) {
  return std::thread([&executor, wait_ms] {
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    executor.dispatch([&executor] {
#ifdef CORO_DEBUG_PROMISE_LEAK
      LOG("debug: debug_coro_leak.size: %zu", debug_coro_promise::debug_coro_leak.size());
      ASSERT(debug_coro_promise::debug_coro_leak.empty());
#endif
      executor.stop();
    });
  });
}
