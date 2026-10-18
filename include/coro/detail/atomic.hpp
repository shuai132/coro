#pragma once

#include <atomic>

#include "coro/detail/atomic_st.hpp"

namespace coro::detail {

// Only coroutine-owned state follows this policy. Cross-thread entry points keep std::atomic.
#ifdef CORO_SINGLE_THREAD
template <typename T>
using atomic = atomic_st<T>;
#else
using std::atomic;
#endif

}  // namespace coro::detail
