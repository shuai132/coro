#pragma once

#include <memory>
#include <utility>

#include "coro/detail/shared_ptr_st.hpp"

namespace coro::detail {

// Only coroutine-owned state uses this policy. Posting callbacks keep their own ownership policy.
#ifdef CORO_SINGLE_THREAD
template <typename T>
using shared_ptr = shared_ptr_st<T>;

template <typename T, typename... ARGS>
shared_ptr<T> make_shared(ARGS&&... args) {
  return shared_ptr_st<T>::make(std::forward<ARGS>(args)...);
}
#else
using std::make_shared;
using std::shared_ptr;
#endif

}  // namespace coro::detail
