#include <iostream>

#include "coro.hpp"

using namespace coro;

async<int> answer() {
  co_return 42;
}

async<void> example() {
  // 子协程自动继承当前执行器。
  int value = co_await answer();
  std::cout << "answer: " << value << '\n';
}

int main() {
  executor_loop executor;
  example().detach(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  return 0;
}
