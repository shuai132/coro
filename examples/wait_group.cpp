#include <chrono>
#include <iostream>

#include "coro.hpp"

using namespace coro;
using namespace std::chrono_literals;

async<void> work(wait_group& group, int id) {
  co_await sleep(1ms);
  std::cout << "finished: " << id << '\n';
  group.done();
}

async<void> example() {
  wait_group group;
  auto* exec = co_await current_executor();
  group.add(2);  // 启动前登记任务，避免过早完成。
  spawn(*exec, work(group, 1));
  spawn(*exec, work(group, 2));
  co_await group.wait();  // 等待全部任务，保证 group 的生命周期。
  std::cout << "all finished\n";
}

int main() {
  executor_loop executor;
  example().detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  return 0;
}
