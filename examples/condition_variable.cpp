#include <iostream>

#include "coro.hpp"

using namespace coro;

async<void> wait_for_data(condition_variable& cv, mutex& mtx, bool& ready) {
  auto guard = co_await mtx.scoped_lock();
  // 带谓词的 wait 会释放锁，唤醒后重新加锁并检查条件。
  co_await cv.wait(mtx, [&] {
    return ready;
  });
  std::cout << "data ready\n";
}

async<void> prepare_data(condition_variable& cv, mutex& mtx, bool& ready) {
  {
    auto guard = co_await mtx.scoped_lock();
    ready = true;
  }
  cv.notify_one();
}

async<void> example() {
  condition_variable cv;
  mutex mtx;
  bool ready = false;
  co_await when_all(wait_for_data(cv, mtx, ready), prepare_data(cv, mtx, ready));
}

int main() {
  executor_loop executor;
  example().detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  return 0;
}
