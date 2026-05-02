#pragma once

#include <coroutine>

#include "coro/executor.hpp"

namespace coro::detail {

inline void resume_handle(std::coroutine_handle<> handle) {
  handle.resume();
}

inline void resume_via(executor* exec, std::coroutine_handle<> handle) {
  if (exec) {
    exec->dispatch([handle]() {
      resume_handle(handle);
    });
  } else {
    resume_handle(handle);
  }
}

}  // namespace coro::detail
