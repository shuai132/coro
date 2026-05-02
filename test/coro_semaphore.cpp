/// config debug
#define CORO_DEBUG_PROMISE_LEAK
#include <vector>

#include "coro.hpp"
#include "detail/assert_def.h"
#include "detail/log.h"
#include "detail/utils.hpp"

using namespace coro;

// Test basic semaphore functionality
async<void> semaphore_basic_test() {
  LOG("=== Basic Semaphore Test ===");
  counting_semaphore sem(3);

  ASSERT(sem.available() == 3);

  // Acquire without blocking
  co_await sem.acquire();
  ASSERT(sem.available() == 2);

  co_await sem.acquire(2);
  ASSERT(sem.available() == 0);

  // Release
  sem.release();
  ASSERT(sem.available() == 1);

  sem.release(2);
  ASSERT(sem.available() == 3);

  LOG("Basic test: OK");
  co_return;
}

// Test try_acquire
async<void> semaphore_try_acquire_test() {
  LOG("=== Try Acquire Test ===");
  counting_semaphore sem(2);

  ASSERT(sem.try_acquire());
  ASSERT(sem.available() == 1);

  ASSERT(sem.try_acquire());
  ASSERT(sem.available() == 0);

  ASSERT(!sem.try_acquire());  // Should fail
  ASSERT(sem.available() == 0);

  sem.release();
  ASSERT(sem.try_acquire());

  LOG("Try acquire test: OK");
  co_return;
}

// Test with multiple waiters
async<void> semaphore_multiple_waiters_test() {
  LOG("=== Multiple Waiters Test ===");
  counting_semaphore sem(2);
  wait_group wg;

  auto worker = [&](int id) -> async<void> {
    LOG("Worker %d waiting for permit", id);
    co_await sem.acquire();
    LOG("Worker %d acquired permit", id);
    co_await sleep(10ms);
    LOG("Worker %d releasing permit", id);
    sem.release();
    wg.done();
    co_return;
  };

  // Spawn 5 workers competing for 2 permits
  wg.add(5);
  co_await spawn_local(worker(1));
  co_await spawn_local(worker(2));
  co_await spawn_local(worker(3));
  co_await spawn_local(worker(4));
  co_await spawn_local(worker(5));

  co_await wg.wait();

  LOG("Multiple waiters test: OK");
  co_return;
}

// Test FIFO behavior when the head waiter needs multiple permits
async<void> semaphore_fifo_waiter_test() {
  LOG("=== FIFO Waiter Test ===");
  counting_semaphore sem(0, 2);
  wait_group wg;
  std::vector<int> order;

  auto first = [&]() -> async<void> {
    co_await sem.acquire(2);
    order.push_back(1);
    sem.release(2);
    wg.done();
    co_return;
  };

  auto second = [&]() -> async<void> {
    co_await sem.acquire();
    order.push_back(2);
    sem.release();
    wg.done();
    co_return;
  };

  wg.add(2);
  co_await spawn_local(first());
  co_await spawn_local(second());

  co_await sleep(10ms);
  sem.release();
  co_await sleep(10ms);
  ASSERT(order.empty());
  ASSERT(sem.available() == 1);

  sem.release();
  co_await wg.wait();

  ASSERT(order.size() == 2);
  ASSERT(order[0] == 1);
  ASSERT(order[1] == 2);
  ASSERT(sem.available() == 2);

  LOG("FIFO waiter test: OK");
  co_return;
}

// Test binary semaphore
async<void> binary_semaphore_test() {
  LOG("=== Binary Semaphore Test ===");
  binary_semaphore sem(1);

  ASSERT(sem.available() == 1);

  co_await sem.acquire();
  ASSERT(sem.available() == 0);

  ASSERT(!sem.try_acquire());

  sem.release();
  ASSERT(sem.available() == 1);

  LOG("Binary semaphore test: OK");
  co_return;
}

async<void> semaphore_scheduled_waiter(counting_semaphore& sem, int& step) {
  co_await sem.acquire();
  ASSERT(step == 1);
  step = 2;
  co_return;
}

async<void> semaphore_release_schedules_waiter_test() {
  LOG("=== Release Schedules Waiter Test ===");
  counting_semaphore sem(0);
  int step = 0;

  co_await spawn_local(semaphore_scheduled_waiter(sem, step));
  co_await sleep(1ms);

  sem.release();
  ASSERT(step == 0);

  step = 1;
  co_await sleep(1ms);
  ASSERT(step == 2);

  LOG("Release schedules waiter test: OK");
  co_return;
}

// Test resource pooling
async<void> semaphore_resource_pool_test() {
  LOG("=== Resource Pool Test ===");
  counting_semaphore sem(3);  // Pool of 3 resources
  int max_concurrent = 0;
  int current_using = 0;
  wait_group wg;

  auto use_resource = [&](int id) -> async<void> {
    co_await sem.acquire();
    current_using++;
    if (current_using > max_concurrent) {
      max_concurrent = current_using;
    }
    LOG("Resource user %d: using (concurrent=%d)", id, current_using);
    co_await sleep(20ms);
    current_using--;
    sem.release();
    LOG("Resource user %d: released", id);
    wg.done();
    co_return;
  };

  // Spawn 10 tasks
  wg.add(10);
  for (int i = 0; i < 10; i++) {
    co_await spawn_local(use_resource(i));
  }

  // Wait for all tasks to complete
  co_await wg.wait();

  // Max concurrent should not exceed semaphore limit
  ASSERT(max_concurrent <= 3);
  ASSERT(sem.available() == 3);

  LOG("Resource pool test: OK (max_concurrent=%d)", max_concurrent);
  co_return;
}

async<void> run_all_tests() {
  co_await semaphore_basic_test();
  co_await semaphore_try_acquire_test();
  co_await binary_semaphore_test();
  co_await semaphore_release_schedules_waiter_test();
  co_await semaphore_multiple_waiters_test();
  co_await semaphore_fifo_waiter_test();
  co_await semaphore_resource_pool_test();

  LOG("=== All Semaphore Tests Passed ===");
  co_return;
}

int main() {
  LOG("Semaphore test init");
  executor_loop executor;
  run_all_tests().detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  check_coro_leak();
  return 0;
}
