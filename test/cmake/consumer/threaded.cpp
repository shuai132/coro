#include <atomic>
#include <thread>

#include "coro/executor_loop.hpp"

int main() {
  coro::executor_loop executor;
  std::atomic<bool> called{false};
  executor.post([&] {
    called.store(true, std::memory_order_release);
    executor.stop();
  });
  std::thread worker([&] {
    executor.run_loop();
  });
  worker.join();
  return called.load(std::memory_order_acquire) ? 0 : 1;
}
