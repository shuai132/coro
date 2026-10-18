#include <iostream>

#include "coro.hpp"

using namespace coro;

async<void> wait_for_signal(event& ready) {
  co_await ready.wait();
  std::cout << "signal received\n";
}

async<void> signal(event& ready) {
  ready.set();  // 唤醒所有等待者，保持置位直到 reset()。
  co_return;
}

async<void> example() {
  event ready;
  co_await when_all(wait_for_signal(ready), signal(ready));
}

int main() {
  executor_loop executor;
  example().detach(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  return 0;
}
