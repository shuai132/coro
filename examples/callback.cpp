#include <iostream>
#include <utility>

#include "coro.hpp"

using namespace coro;

async<void> example() {
  int value = co_await callback_awaiter<int>([](executor* exec, auto done) {
    // 用执行器模拟异步 API；完成时调用 done(result)。
    exec->post([done = std::move(done)] {
      done(42);
    });
  });
  std::cout << "callback result: " << value << '\n';
}

int main() {
  executor_loop executor;
  example().detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  return 0;
}
