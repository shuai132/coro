#pragma once

#include <new>
#include <thread>
#include <type_traits>
#include <utility>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "coro/executor_poll.hpp"

namespace retained_child_test {

struct submission_executor : coro::executor_poll {
  bool reject_ = false;

  void post(std::function<void()> fn) override {
#ifndef CORO_DISABLE_EXCEPTION
    if (reject_) {
      throw std::bad_alloc();
    }
#endif
    executor_poll::post(std::move(fn));
  }
};

struct frame_owner {
  int* live_;

  explicit frame_owner(int& live) : live_(&live) {
    ++*live_;
  }
  frame_owner(frame_owner&& other) noexcept : live_(std::exchange(other.live_, nullptr)) {}
  ~frame_owner() {
    if (live_) {
      --*live_;
    }
  }
};

template <typename T>
coro::async<T> child([[maybe_unused]] frame_owner owner, int& starts) {
  ++starts;
  if constexpr (!std::is_void_v<T>) {
    co_return 42;
  } else {
    co_return;
  }
}

template <typename T>
coro::async<void> parent(coro::async<T>& task, bool& completed, [[maybe_unused]] bool reject) {
#ifndef CORO_DISABLE_EXCEPTION
  bool caught = false;
  try {
#endif
    if constexpr (std::is_void_v<T>) {
      co_await task;
    } else {
      ASSERT((co_await task) == 42);
    }
#ifndef CORO_DISABLE_EXCEPTION
  } catch (const std::bad_alloc&) {
    caught = true;
  }
  ASSERT(caught == reject);
#endif
  completed = true;
}

template <typename T>
void run(bool reject, bool worker) {
  coro::executor_poll parent_executor;
  submission_executor child_executor;
  child_executor.reject_ = reject;
  int live = 0;
  int starts = 0;
  auto task = child<T>(frame_owner{live}, starts).bind_executor(child_executor);
  bool completed = false;
  coro::spawn(parent_executor, parent(task, completed, reject));
  parent_executor.poll_one();
  if (!reject) {
    child_executor.poll_one();
    parent_executor.poll_one();
  }
  ASSERT(completed);
  ASSERT(live == 1);
  ASSERT(starts == (reject ? 0 : 1));

#ifndef CORO_DISABLE_EXCEPTION
  if (reject) {
    // Reusing the retained child must not retain any of the failed parents.
    for (int attempt = 0; attempt < 2; ++attempt) {
      bool retried = false;
      coro::spawn(parent_executor, parent(task, retried, true));
      parent_executor.poll_one();
      ASSERT(retried);
      ASSERT(live == 1 && starts == 0);
    }
  }
#endif

  // The first parent has finished and released its frame; the retained child still belongs to task.
  child_executor.reject_ = false;
  task.detach(child_executor);
  if (worker) {
    std::thread thread([&child_executor] {
      child_executor.poll_one();
    });
    thread.join();
  } else {
    child_executor.poll_one();
  }
  parent_executor.poll_one();
  ASSERT(starts == 1);
  ASSERT(live == 0);
}

}  // namespace retained_child_test
