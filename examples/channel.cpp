#include <iostream>

#include "coro.hpp"

using namespace coro;

async<void> produce(channel<int>& ch) {
  co_await ch.send(42);
  ch.close();
}

async<void> consume(channel<int>& ch) {
  // 关闭且无剩余数据时，recv() 返回空 optional。
  while (auto value = co_await ch.recv()) {
    std::cout << "received: " << *value << '\n';
  }
}

async<void> example() {
  channel<int> ch;  // 默认无缓冲，发送方等待接收方。
  co_await when_all(produce(ch), consume(ch));
}

int main() {
  executor_loop executor;
  example().detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  return 0;
}
