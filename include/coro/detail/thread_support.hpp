#pragma once

#include <mutex>

// Threadless standard libraries still provide generic lock guards, but not std::mutex.
// Keep explicit mutex template arguments and the _st aliases usable in that case.
#if (defined(__GLIBCXX__) && !defined(_GLIBCXX_HAS_GTHREADS)) || defined(_LIBCPP_HAS_NO_THREADS) || \
    (defined(_LIBCPP_VERSION) && defined(_LIBCPP_HAS_THREADS) && !_LIBCPP_HAS_THREADS)
#define CORO_DETAIL_HAS_STD_MUTEX 0
#else
#define CORO_DETAIL_HAS_STD_MUTEX 1
#endif
