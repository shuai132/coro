#pragma once

// Diagnostics must not interrupt ownership transfer, destruction, or final suspension.
// Use compiler exception support: CORO_DISABLE_EXCEPTION only disables body propagation.
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#define CORO_DETAIL_LOG_NOEXCEPT(LOG, ...) \
  do {                                     \
    try {                                  \
      LOG(__VA_ARGS__);                    \
    } catch (...) {                        \
    }                                      \
  } while (false)
#else
#define CORO_DETAIL_LOG_NOEXCEPT(LOG, ...) \
  do {                                     \
    LOG(__VA_ARGS__);                      \
  } while (false)
#endif
