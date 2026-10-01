#include <chrono>
#include <iostream>

#include "coro.hpp"

using namespace coro;
using namespace std::chrono_literals;

async<void> example() {
  std::cout << "waiting\n";
  co_await sleep(10ms);  // 挂起协程，执行器可以继续运行其他任务。
  std::cout << "awake\n";
}

int main() {
  executor_loop executor;
  example().detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  return 0;
}
