# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**coro** is a lightweight, header-only C++20 coroutine library with async tasks, concurrency control, and synchronization primitives. It is designed to be simple, multi-platform (including embedded/MCU), and optionally exception-free.

## Build Commands

```bash
# Configure and build
mkdir build && cd build
cmake ..
make

# Build with sanitizers
cmake .. -DCORO_ENABLE_SANITIZE_ADDRESS=ON   # AddressSanitizer
cmake .. -DCORO_ENABLE_SANITIZE_THREAD=ON    # ThreadSanitizer

# Build without exceptions
cmake .. -DCORO_DISABLE_EXCEPTION=ON
```

## Running Tests

Each test is a separate executable. Run individual tests from the build directory:

```bash
./coro_task              # Core async task tests
./coro_task_verbose      # Task tests with debug lifecycle logging
./coro_mutex             # Mutex tests
./coro_channel           # Channel tests
./coro_when              # when_all/when_any tests
./coro_condition_variable    # Condition variable tests
./coro_condition_variable_mt # Multi-threaded CV tests
./coro_semaphore         # Semaphore tests
./coro_semaphore_mt      # Multi-threaded semaphore tests
./coro_wait_group        # Wait group tests
./coro_wait_group_mt     # Multi-threaded wait group tests
./coro_latch             # Latch tests
./coro_event             # Event tests
./coro_broadcast         # Channel broadcast tests
./coro_multi_thread      # Multi-thread tests
./coro_multi_thread_st   # Multi-thread tests (single-thread mode)
```

## Architecture

### Core Components

- **`async<T>`** (`include/coro/coro.hpp`): The primary coroutine type, aliased from `awaitable<T>`. Supports `co_await`, `co_return`, and lifecycle management. Coroutines are lazily started and inherit their executor from the caller.

- **`executor`** (`include/coro/executor.hpp`): Abstract interface with four methods: `dispatch()`, `post()`, `post_delayed_ns()`, and `stop()`. Two implementations:
  - `executor_loop`: Condition variable-based event loop for blocking execution
  - `executor_poll`: Non-blocking polling mode for integration with existing event loops

### Spawning Coroutines

- `spawn(executor, awaitable)`: Launch a detached coroutine on a specific executor
- `spawn_local(awaitable)`: Launch on the current coroutine's executor (must be `co_await`ed)
- `awaitable.detach(executor)`: Alternative detach syntax

### Synchronization Primitives (all in `include/coro/`)

| Header | Type | Key Design |
|--------|------|------------|
| `mutex.hpp` | `mutex` | Coroutine-safe, uses `co_await mtx.scoped_lock()` |
| `channel.hpp` | `channel<T>` | Go-style, buffered/unbuffered, supports `broadcast()` |
| `semaphore.hpp` | `counting_semaphore`, `binary_semaphore` | `acquire()` suspends, `release()` is non-blocking |
| `condition_variable.hpp` | `condition_variable` | Works with `coro::mutex` |
| `event.hpp` | `event` | One-shot or resettable signal |
| `latch.hpp` | `latch` | Countdown synchronization |
| `wait_group.hpp` | `wait_group` | Go-style `add()`/`done()`/`wait()` |

### Concurrency Operations

- `when_all(awaitables...)`: Returns tuple of non-void results
- `when_any(awaitables...)`: Returns result with `.index` and `.get<N>()`

### Callback Integration

`callback_awaiter<T>` converts callback-style APIs to coroutines.

## Key Design Decisions

1. **Lazy coroutines**: All coroutines start suspended at `initial_suspend()` and are resumed when awaited or detached.

2. **Executor inheritance**: Child coroutines inherit the executor from their parent unless explicitly bound via `bind_executor()`.

3. **Non-blocking sync primitive operations**: Methods like `mutex.unlock()`, `semaphore.release()`, `event.set()` can be called from any context (not just coroutines).

4. **Optional exception support**: Define `CORO_DISABLE_EXCEPTION` for embedded platforms without exception support.

## Debug Macros

```cpp
#define CORO_DEBUG_PROMISE_LEAK      // Track coroutine promise allocations
#define CORO_DEBUG_LEAK_LOG printf   // Log function for leak detection
#define CORO_DEBUG_LIFECYCLE printf  // Log coroutine lifecycle events
```

Call `debug_coro_promise::dump()` to check for leaks when `CORO_DEBUG_PROMISE_LEAK` is defined.
