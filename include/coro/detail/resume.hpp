#pragma once

#include <coroutine>

#include "coro/executor.hpp"

namespace coro::detail {

inline void resume_handle(std::coroutine_handle<> handle) {
  handle.resume();
}

// A trivial target lets built-in queues retain a continuation without owning a user callback.
struct resume_task {
  std::coroutine_handle<> handle_;

  void operator()() const {
    resume_handle(handle_);
  }
};

inline void resume_via(executor* exec, std::coroutine_handle<> handle) {
  if (exec) {
    exec->dispatch(resume_task{handle});
  } else {
    resume_handle(handle);
  }
}

inline void post_resume_via(executor* exec, std::coroutine_handle<> handle) {
  if (exec) {
    exec->post_continuation(resume_task{handle});
  } else {
    resume_handle(handle);
  }
}

}  // namespace coro::detail
