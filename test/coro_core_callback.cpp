#include <functional>
#include <memory>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "coro/executor_loop.hpp"

using namespace coro;

struct callback_state {
  int call_depth = 0;
  int destructor_during_call = 0;
};

struct synchronous_callback {
  std::shared_ptr<callback_state> state;

  ~synchronous_callback() {
    if (state && state->call_depth > 0) {
      ++state->destructor_during_call;
    }
  }

  void operator()(std::function<void(int)> callback) const {
    ++state->call_depth;
    callback(7);
    --state->call_depth;
  }
};

async<void> await_synchronous_callback(std::shared_ptr<callback_state> state) {
  int value = co_await callback_awaiter<int>(synchronous_callback{state});
  ASSERT(value == 7);
}

int main() {
  executor_loop executor;
  auto state = std::make_shared<callback_state>();

  await_synchronous_callback(state).detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();

  ASSERT(state->destructor_during_call == 0);
  return 0;
}
