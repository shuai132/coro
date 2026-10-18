#include <chrono>
#include <iostream>

#include "coro.hpp"

using namespace coro;
using namespace std::chrono_literals;

async<int> value_after(int value, std::chrono::milliseconds delay) {
  co_await sleep(delay);
  co_return value;
}

async<int> tracked_value(wait_group& group, int value, std::chrono::milliseconds delay) {
  int result = co_await value_after(value, delay);
  group.done();
  co_return result;
}

async<void> example() {
  auto [first, second] = co_await when_all(value_after(10, 1ms), value_after(20, 2ms));
  std::cout << "all: " << first << ", " << second << '\n';

  wait_group group;
  group.add(2);
  auto winner = co_await when_any(tracked_value(group, 10, 1ms), tracked_value(group, 20, 2ms));
  std::cout << "first completed index: " << winner.index << '\n';
  // when_any 不取消其他任务；退出作用域前等待它们结束。
  co_await group.wait();
}

int main() {
  executor_loop executor;
  example().detach(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  return 0;
}
