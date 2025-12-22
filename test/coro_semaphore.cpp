/// config debug
#define CORO_DEBUG_PROMISE_LEAK
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

  auto exec = co_await current_executor();

  // Spawn 5 workers competing for 2 permits
  wg.add(5);
  co_spawn(*exec, worker(1));
  co_spawn(*exec, worker(2));
  co_spawn(*exec, worker(3));
  co_spawn(*exec, worker(4));
  co_spawn(*exec, worker(5));

  co_await wg.wait();

  LOG("Multiple waiters test: OK");
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

  auto exec = co_await current_executor();

  // Spawn 10 tasks
  wg.add(10);
  for (int i = 0; i < 10; i++) {
    co_spawn(*exec, use_resource(i));
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
  co_await semaphore_multiple_waiters_test();
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
