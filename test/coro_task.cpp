/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include "log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include "TimeCount.hpp"
#include "assert_def.h"
#include "coro/coro.hpp"
#include "coro/time.hpp"
#include "utils.hpp"

/// config executor
#define CORO_EXECUTOR_LOOP
#define CORO_EXECUTOR_POLL

#ifdef CORO_EXECUTOR_LOOP
#include "coro/executor_loop.hpp"
#elif defined(CORO_EXECUTOR_POLL)
#include "coro/executor_poll.hpp"
#endif

using namespace coro;

async<int> coro_fun() {
  {
    LOG("sleep 500ms begin");
    TimeCount t;
    co_await sleep(500ms);
    ASSERT(int(t.elapsed()) >= 500);
    LOG("sleep 500ms end: %d", (int)t.elapsed());
  }

  LOG("callback_awaiter begin");
  auto ret = co_await callback_awaiter<int>([](auto callback) {
    std::thread([callback = std::move(callback)] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      callback(123);
    }).detach();
  });
  ASSERT(ret == 123);
  LOG("callback_awaiter end");
  co_return ret;
}

async<void> coro_task() {
  int ret = co_await coro_fun();
  LOG("coro_fun ret: %d", ret);
  ASSERT(ret == 123);
}

async<int> coro_task_int() {
  LOG("coro_task_int 1");
  co_return 1;
}

async<int> coro_task_exception([[maybe_unused]] bool rethrow = false) {
#ifndef CORO_DISABLE_EXCEPTION
  bool flag = false;
  try {
    auto v = co_await []() -> async<int> {
      throw std::runtime_error("coro_task_exception");
      co_return 1;
    }();
    co_return v;
  } catch (std::exception& e) {
    LOG("exception: %s", e.what());
    if (rethrow) {
      std::rethrow_exception(std::current_exception());
    }
    flag = true;
  }
  ASSERT(flag);
#endif

  co_return co_await []() -> async<int> {
    co_return 2;
  }();
}

async<void> loop_task(const char* tag, int ms) {
  int count = 3;
  while (count--) {
    TimeCount t;
    co_await sleep(std::chrono::milliseconds(ms));
    LOG("%s: %d, elapsed: %d", tag, ms, (int)t.elapsed());
    ASSERT(int(t.elapsed()) >= ms);
  }
}

void test_coro(executor& executor) {
  co_spawn(executor, loop_task("A", 100));
  co_spawn(executor, loop_task("B", 200));
  co_spawn(executor, loop_task("C", 300));

  co_spawn(executor, coro_task());
  // or: coro_task().detach(executor);

  coro_task_int().detach(executor);

  coro_task().detach_with_callback(executor, [&] {
    LOG("coro_task finished");
  });

  coro_task_exception().detach_with_callback(executor, [&](int v) {
    LOG("coro_task_exception finished: %d", v);
    ASSERT(v == 2);
  });

#ifndef CORO_DISABLE_EXCEPTION
  coro_task_exception(true).detach_with_callback(
      executor,
      [&](int) {
        ASSERT(false);
      },
      [](const std::exception_ptr& e) {
        try {
          std::rethrow_exception(e);
        } catch (const std::runtime_error& e) {
          LOG("coro_task_exception(true) exception: %s", e.what());
          ASSERT(std::string_view(e.what()) == "coro_task_exception");
        }
      });
#endif
}

void test_simple(executor& executor) {
  co_spawn(executor, [](auto e) -> async<void> {
    ASSERT(e == co_await current_executor());
    TimeCount t;
    const auto ms = 100;
    co_await sleep(std::chrono::milliseconds(ms));
    LOG("%s: %d, elapsed: %d", "test_simple", ms, (int)t.elapsed());
    ASSERT(int(t.elapsed()) >= ms);
  }(&executor));
}

int main() {
  LOG("init");
#ifdef CORO_EXECUTOR_LOOP
  executor_loop executor;
#elif defined(CORO_EXECUTOR_POLL)
  executor_poll executor;
#endif

  test_coro(executor);
  test_simple(executor);
  delay_stop(executor, 1500);
  LOG("loop...");
#ifdef CORO_EXECUTOR_LOOP
  executor.run_loop();
#elif defined(CORO_EXECUTOR_POLL)
  while (!executor.stopped()) {
    executor.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
#endif
  check_coro_leak();
  return 0;
}
