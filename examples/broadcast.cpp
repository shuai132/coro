#include <iostream>

#include "coro.hpp"

using namespace coro;

async<void> receive(channel<int>& ch, int id) {
  if (auto value = co_await ch.recv()) {
    std::cout << "receiver " << id << ": " << *value << '\n';
  }
}

async<void> broadcast(channel<int>& ch) {
  auto count = co_await ch.broadcast(42);
  std::cout << "notified: " << count << '\n';
}

int main() {
  executor_poll executor;
  channel<int> ch;
  int finished = 0;
  auto on_finished = [&] {
    ++finished;
  };
  for (int id = 1; id <= 2; ++id) {
    receive(ch, id).detach(executor, on_finished);
    executor.poll_one();  // 广播只发给当前等待者，先让接收协程进入等待。
  }
  broadcast(ch).detach(executor, on_finished);
  while (finished != 3) {
    executor.poll_one();  // 适合由应用自己的循环驱动协程。
  }
  return 0;
}
