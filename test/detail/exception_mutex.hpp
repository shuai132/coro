#pragma once

#include <mutex>
#include <stdexcept>
#include <utility>

#include "assert_def.h"

struct exception_mutex {
  inline static thread_local bool fail_lock = false;
  std::mutex mutex_;

  void lock() {
#ifndef CORO_DISABLE_EXCEPTION
    if (std::exchange(fail_lock, false)) {
      throw std::runtime_error("mutex lock failed");
    }
#endif
    mutex_.lock();
  }
  void unlock() noexcept {
    mutex_.unlock();
  }
};

#ifndef CORO_DISABLE_EXCEPTION
template <typename FUNCTION>
void expect_lock_failure(FUNCTION function) {
  exception_mutex::fail_lock = true;
  bool caught = false;
  try {
    function();
  } catch (const std::runtime_error&) {
    caught = true;
  }
  ASSERT(caught);
  ASSERT(!exception_mutex::fail_lock);
}
#endif
