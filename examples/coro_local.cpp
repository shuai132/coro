#ifndef CORO_ENABLE_LOCAL_STORAGE
#define CORO_ENABLE_LOCAL_STORAGE
#endif

#include <iostream>

#include "coro.hpp"

using namespace coro;

static coro_local<int> request_id;

async<void> child() {
  // 子协程继承父协程的本地存储。
  std::cout << "request: " << co_await request_id.get() << '\n';
}

async<void> example() {
  co_await request_id.set(42);
  co_await child();
}

int main() {
  executor_loop executor;
  example().detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  return 0;
}
