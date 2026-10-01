#include <chrono>
#include <iostream>

#include "coro.hpp"

using namespace coro;
using namespace std::chrono_literals;

async<void> increment(mutex& mtx, int& value) {
  auto guard = co_await mtx.scoped_lock();
  // 挂起期间仍持有锁，另一个协程需要等待。
  co_await sleep(1ms);
  ++value;
}  // guard 析构时自动解锁。

async<void> example() {
  mutex mtx;
  int value = 0;
  co_await when_all(increment(mtx, value), increment(mtx, value));
  std::cout << "value: " << value << '\n';
}

int main() {
  executor_loop executor;
  example().detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  return 0;
}
