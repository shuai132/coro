#include <memory>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "coro/executor_loop.hpp"

using namespace coro;

async<std::unique_ptr<int>> make_unique_value() {
  co_return std::make_unique<int>(42);
}

async<void> consume_unique_value(int& result) {
  auto value = co_await make_unique_value();
  ASSERT(value);
  result = *value;
}

int main() {
  executor_loop executor;
  int result = 0;

  consume_unique_value(result).detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();

  ASSERT(result == 42);
  return 0;
}
