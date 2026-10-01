#include <chrono>
#include <iostream>

#include "coro.hpp"

using namespace coro;
using namespace std::chrono_literals;

async<void> work(counting_semaphore& slots, int id) {
  co_await slots.acquire();
  std::cout << "working: " << id << '\n';
  co_await sleep(1ms);
  slots.release();  // 归还许可；若工作可能失败，也应在失败路径归还。
}

async<void> example() {
  counting_semaphore slots(2);  // 最多两个协程同时工作。
  co_await when_all(work(slots, 1), work(slots, 2), work(slots, 3));
}

int main() {
  executor_loop executor;
  example().detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  return 0;
}
