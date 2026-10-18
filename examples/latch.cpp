#include <iostream>

#include "coro.hpp"

using namespace coro;

async<void> participant(latch& ready, int id) {
  std::cout << "ready: " << id << '\n';
  co_await ready.arrive_and_wait();  // 计数减一，等待所有参与者到齐。
  std::cout << "started: " << id << '\n';
}

async<void> example() {
  latch ready(2);  // 一次性同步点，不能重置。
  co_await when_all(participant(ready, 1), participant(ready, 2));
}

int main() {
  executor_loop executor;
  example().detach(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  return 0;
}
