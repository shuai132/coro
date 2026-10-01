#pragma once

// Exception-enabled diagnostics must not interrupt ownership transfer, destruction, or final suspension.
// CORO_DISABLE_EXCEPTION removes the guard too; logging hooks must then be nonthrowing.
#ifndef CORO_DISABLE_EXCEPTION
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
