# coro

[![CI](https://github.com/shuai132/coro/actions/workflows/ci.yml/badge.svg)](https://github.com/shuai132/coro/actions/workflows/ci.yml)
<img alt="feature" src="https://img.shields.io/badge/c++20-Coroutines-orange">
<img alt="language" src="https://img.shields.io/github/languages/top/shuai132/coro">

A lightweight C++20 coroutine library with async tasks, concurrency control, and synchronization primitives.

[中文文档](README_CN.md)

## Table of Contents

- [Preface](#preface)
- [API Overview](#api-overview)
- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Executors](#executors)
- [Timer](#timer)
- [Concurrency Operations](#concurrency-operations)
- [Synchronization Primitives](#synchronization-primitives)
    - [mutex](#mutex)
    - [condition_variable](#condition_variable)
    - [semaphore](#semaphore)
    - [channel](#channel)
    - [wait_group](#wait_group)
    - [latch](#latch)
    - [event](#event)
- [Coroutine-Local Storage](#coroutine-local-storage)
- [Callback to Coroutine](#callback-to-coroutine)
- [ASIO Adaptor](#asio-adaptor)
- [Configuration Options](#configuration-options)
- [Building Tests](#building-tests)
- [Project Structure](#project-structure)

## Preface

This project was initially created for learning C++20 coroutines. During some free time on weekends, I decided to
improve it by adding necessary APIs and synchronization primitives, and it has now become very comprehensive.

### Design Goals:

* Clear process, simple and easy to understand (hopefully so)

  C++20's coroutine design is very obscure, with APIs mainly designed for library developers. For any coroutine library,
  to understand its design, one must first understand the process and behavior of coroutine APIs.
  Therefore, it may not really be simple and easy to understand, but relatively speaking, this library tries not to use
  obscure templates, concept constraints, type nesting, or switching jump behaviors.


* Multi-platform support

  Embedded platform support, even usable on MCUs, is one of the design goals! On some platforms without general-purpose
  OS, or even without RTOS, and even without exception support, many open-source libraries cannot be used.
  And the compiler requirements are relatively high; some complex features are not fully supported by some compilers,
  even if the GCC version appears to be high.


* Bug-free (especially memory and threading issues)

  I found that many open-source libraries have surprisingly simple unit tests, especially in multi-threading where
  testing is almost non-existent. They also lack automated testing for race conditions and memory leaks (based on
  Sanitize), and their quality relies too heavily on user feedback. Even with high prominence, the engineering quality
  is not good enough.

I have also learned about some well-known C++20 coroutine open-source libraries, such as:

* [https://github.com/alibaba/async_simple](https://github.com/alibaba/async_simple)
* [https://github.com/jbaldwin/libcoro](https://github.com/jbaldwin/libcoro)

### Why reinventing the wheel:

* First, the design goals are different, as mentioned above.


* Another major reason is the design trade-offs. I want to prioritize **ease of understand and use** in API design,
  feature design, and implementation.

  For example, `libcoro` supports `co_await tp->schedule()` and recommends it as the paradigm for thread switching,
  which I think is extremely inappropriate. Switching threads within the same code block context is very
  counter-intuitive and error-prone.

  For example, the synchronization primitive design in `async_simple` and `libcoro` requires users to call them in
  coroutine context, such as `co_await semaphore.release()`.
  I believe looser constraints are easier to use, allowing users to call `semaphore.release()` anywhere.

  There are many similar design trade-offs, not listing them one by one.


* They are somewhat cumbersome in the design of coroutine types and behaviors.

  For example, to detach a coroutine, one has to go through multiple layers of wrapping. This is not a major issue, but
  I think it's completely unnecessary. This would go through multiple coroutine creation-to-destruction lifecycles,
  making it difficult to troubleshoot issues.


* Summary

  These open-source libraries all have their unique designs. After later seeing the implementation of `async_simple`, I
  was surprised to find that many designs are very similar! But the details and trade-offs are different.

## API Overview

| Name                                         | Description                                           |
|----------------------------------------------|-------------------------------------------------------|
| `coro::async<T>`                             | Async task type, supports `co_await` and `co_return`  |
| `coro::spawn(executor, awaitable)`           | Spawn a coroutine on an executor                      |
| `coro::spawn_local(awaitable)`               | Spawn a coroutine on the current executor             |
| `coro::when_all(awaitables...) -> awaitable` | Wait for all tasks to complete                        |
| `coro::when_any(awaitables...) -> awaitable` | Wait for any task to complete                         |
| `coro::sleep(duration)`                      | Async wait for specified duration (chrono duration)   |
| `coro::delay(ms)`                            | Async wait for specified milliseconds                 |
| `co_await chrono_duration`                   | Async wait directly (e.g., `co_await 1s`)             |
| `coro::mutex`                                | Coroutine-safe mutex                                  |
| `coro::condition_variable`                   | Coroutine-safe condition variable for synchronization |
| `coro::event`                                | Event synchronization primitive                       |
| `coro::latch`                                | Countdown latch for synchronization                   |
| `coro::counting_semaphore`                   | Counting semaphore for resource control               |
| `coro::wait_group`                           | Wait group for coordinating multiple coroutines       |
| `coro::channel<T>`                           | Go-style channel for inter-coroutine communication    |
| `coro::executor`                             | Executor base class interface                         |
| `coro::executor_loop`                        | Event loop based executor                             |
| `coro::executor_poll`                        | Polling based executor                                |
| `coro::current_executor()`                   | Get current executor                                  |
| `coro::callback_awaiter<T>`                  | Convert callback-style APIs to coroutines             |
| `coro::coro_local<T>`                        | Coroutine-local storage (like thread_local)           |
| `coro::inherit_coro_local()`                 | Ensure local storage exists                           |

## Features

- 🚀 **Header-only**: No compilation required, just include and use
- 📦 **C++20 Standard**: Built on C++20 coroutine features
- 🔄 **Async Tasks (async/awaitable)**: Support `co_await` and `co_return`
- ⏰ **Timer Support**: Built-in `sleep` and `delay` async waiting
- 🔀 **Concurrency Primitives**: Support `when_all` and `when_any` operations
- 📨 **Channel**: Go-style channels with buffered and unbuffered modes
- 🔒 **Mutex**: Coroutine-safe mutex with RAII-style `scoped_lock`
- 🎛️ **Executors**: Polling mode (`executor_poll`), event loop mode (`executor_loop`), or custom implementation
- ⚠️ **Exception Support**: Optional exception handling, can be disabled via macro
- 🛠️ **Debug Support**: Built-in coroutine leak detection
- 🔍 **Unit Tests**: Comprehensive unit and integration tests
- 📦 **Embedded Support**: Compatible with MCU and embedded platforms
- 🧩 **Extended Synchronization Primitives**: Additional synchronization tools including condition variables, events,
  latches, semaphores, and wait groups
- 🗂️ **Coroutine-Local Storage**: Thread-local-like storage scoped to coroutines with inheritance support

## Requirements

- A compiler and standard library with working C++20 coroutine support (`<coroutine>` and `std::coroutine_handle`)
- CMake 3.15+ (optional, for building tests)

GCC builds need tail-call elimination for bounded stack usage across repeated immediate `co_await` operations.
The CMake `coro` target enables `-foptimize-sibling-calls` for GCC consumers, including Debug builds; add it manually
when using the headers directly. This option also enables tail-call optimization in the consuming target's other C++
functions and can change debugger backtraces. Verify stack usage on your target ABI and toolchain.

GCC 10 also requires `-fcoroutines`; the CMake `coro` target supplies this option automatically.

GCC 10.3 does not pass the coroutine regression suite: some lvalue awaiters fail to compile, and temporary aggregate awaiters can release shared ownership prematurely. Use a toolchain that passes the test suite; C++20 mode alone is insufficient to establish compatibility.

On bare-metal standard libraries without `std::mutex`, include individual component headers (for example,
`coro/channel.hpp`) and use `_st` aliases or supply an explicit mutex template argument. Native-mutex defaults and
`_mt` aliases are unavailable on these toolchains. Provide a platform executor and define `CORO_DISABLE_EXCEPTION`
when compiling without exceptions. The umbrella `coro.hpp`, bundled executors, `when_*`, and promise leak tracing
currently require standard threading support.

## Installation

### Option 1: Direct Include

As a header-only library, simply add the `include` directory to your project's include path:

```cpp
#include "coro.hpp"
```

### Option 2: CMake

```cmake
add_subdirectory(coro)
target_link_libraries(your_target coro)
```

## Quick Start

### Basic Usage

`async<T>` supports both expressions and braced initialization in `co_return`, including move-only aggregate results.

```cpp
#include <iostream>
#include "coro/coro.hpp"
#include "coro/time.hpp"
#include "coro/executor_loop.hpp"

using namespace coro;

// Define an async task returning int
async<int> fetch_data() {
    co_await sleep(100ms);  // Async wait for 100 milliseconds
    co_return 42;
}

// Define a void async task
async<void> process() {
    int data = co_await fetch_data();
    std::cout << "Data: " << data << std::endl;
}

int main() {
    executor_loop executor;

    // Launch the task and stop the event loop when it finishes
    process().detach_with_callback(executor, [&executor] {
        executor.stop();
    });
    
    // Run event loop
    executor.run_loop();
    return 0;
}

// Using spawn_local inside coroutine
async<void> main_task() {
    co_await spawn_local(process());
}
```

### Launch Coroutine with Callback

```cpp
async<int> compute() {
    co_await sleep(50ms);
    co_return 123;
}

// Handle result with callback
compute().detach_with_callback(
    executor,
    [](int result) {
        std::cout << "Result: " << result << std::endl;
    },
    [](std::exception_ptr ex) {
        // Optional exception handling
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }
);
```

### Get Current Executor

```cpp
async<void> example() {
    executor* exec = co_await current_executor();
    // Use exec...
}
```

`bind_executor(exec)` selects a task's executor. When directly awaiting a child bound to another executor, the child runs on its selected executor and the caller continues on its own executor. Unbound children inherit their caller's executor.
These direct cross-executor awaits queue both child starts and caller continuations, avoiding recursive dispatch when the executors share a thread.

## Executors

Two executor implementations are provided:

The built-in executors and `asio_executor` saturate unrepresentable `post_delayed_ns()` deadlines at the steady clock's maximum time point.

The built-in executors service both ready timers and posted tasks, so a continuously replenished queue cannot starve the other.

### executor_loop

Condition variable based event loop, suitable for main thread:

```cpp
#include "coro/executor_loop.hpp"

executor_loop executor;

// Launch coroutines...

// Block until stop() is called
executor.run_loop();
```

After `stop()`, `run_loop()` finishes already queued tasks, including timers, before returning.

### executor_poll

Non-blocking polling mode, suitable for integration with existing event loops:

```cpp
#include "coro/executor_poll.hpp"

executor_poll executor;

// Launch coroutines...

// Call in your main loop
while (!executor.stopped()) {
    executor.poll();
    // Other work...
    std::this_thread::sleep_for(10ms);
}
```

### Custom Executor

Inherit from `coro::executor` interface to implement custom executors:

```cpp
struct my_executor : coro::executor {
    void dispatch(std::function<void()> fn) override;      // Execute immediately or later
    void post(std::function<void()> fn) override;          // Execute later
    void post_delayed_ns(std::function<void()> fn, uint64_t delay_ns) override;  // Delayed execution
    void stop() override;                                  // Stop executor
};
```

## Timer

```cpp
#include "coro/time.hpp"

async<void> timer_example() {
    // Using chrono duration
    co_await sleep(100ms);
    co_await sleep(std::chrono::seconds(1));
    
    // Or using milliseconds
    co_await delay(500);  // 500 milliseconds
}
```

`sleep(duration)` and direct `co_await duration` submit non-positive durations to the executor as zero-delay operations.
`sleep(duration)` and `delay(ms)` saturate positive values that exceed the nanosecond representation at `std::chrono::nanoseconds::max()`.

## Concurrency Operations

When bound to an executor, nonempty `when_all()` and `when_any()` queue their initial child launch and the resumption
of suspended callers. This keeps deeply nested combinations from growing the native call stack.

### when_all

Wait for all tasks to complete:

With exceptions enabled, `when_all()` waits for all launched children before rethrowing the first recorded error.
If launching a child fails, subsequent children are not started.

```cpp
#include "coro/when.hpp"

async<int> task1() { co_await sleep(100ms); co_return 1; }
async<int> task2() { co_await sleep(50ms);  co_return 2; }
async<void> task3() { co_await sleep(75ms); }

async<void> example() {
    // Wait for all tasks, returns tuple of non-void results
    auto [r1, r2] = co_await when_all(task1(), task2(), task3());
    // r1 = 1, r2 = 2
    // task3 is void type, not included in result
    
    // If all tasks are void type
    co_await when_all(task3(), task3());
    
    // If only one non-void task, returns value directly (not tuple)
    int result = co_await when_all(task3(), task1(), task3());
    // result = 1
}
```

### when_any

Wait for any task to complete:

```cpp
async<void> example() {
    // Returns first completed task
    auto result = co_await when_any(task1(), task2(), task3());
    
    // result.index indicates completed task index
    std::cout << "Task " << result.index << " completed first" << std::endl;
    
    // Get completed task's return value (if not void)
    if (result.index == 0) {
        int value = result.template get<0>();
    } else if (result.index == 1) {
        int value = result.template get<1>();
    }
    // index == 2's task3 is void type
}
```

`when_any()` requires at least one awaitable. It returns when the first task completes and does not cancel the remaining
tasks; slower tasks continue running on their executors.

## Mutex

Coroutine-safe mutex:

### Using scoped_lock (Recommended)

```cpp
#include "coro/mutex.hpp"

coro::mutex mtx;

async<void> critical_section() {
    {
        auto guard = co_await mtx.scoped_lock();
        // Critical section code
        // ...
    }  // Auto unlock
}
```

### Manual lock/unlock

```cpp
async<void> manual_lock() {
    co_await mtx.lock();
    // Critical section code
    mtx.unlock();
}
```

### Early Unlock

```cpp
async<void> early_unlock() {
    auto guard = co_await mtx.scoped_lock();
    // Critical section code...
    
    guard.unlock();  // Manual early unlock

    // Non-critical section code...
}
```

## Synchronization Primitives

The library provides several coroutine-safe synchronization primitives:

### mutex

Coroutine-safe mutex:

#### Using scoped_lock (Recommended)

```cpp
#include "coro/mutex.hpp"

coro::mutex mtx;

async<void> critical_section() {
    {
        auto guard = co_await mtx.scoped_lock();
        // Critical section code
        // ...
    }  // Auto unlock
}
```

The mutex must be unlocked before it is destroyed, and waiting coroutines must remain alive until they are resumed.

#### Manual lock/unlock

```cpp
async<void> manual_lock() {
    co_await mtx.lock();
    // Critical section code
    mtx.unlock();
}
```

#### Early Unlock

```cpp
async<void> early_unlock() {
    auto guard = co_await mtx.scoped_lock();
    // Critical section code...

    guard.unlock();  // Manual early unlock

    // Non-critical section code...
}
```

### condition_variable

Coroutine-safe condition variable, similar to Go's `sync.Cond`. Must be used with `coro::mutex`.

```cpp
#include "coro/condition_variable.hpp"
#include "coro/mutex.hpp"

coro::condition_variable cv;
coro::mutex mtx;
bool ready = false;

async<void> waiter() {
    auto guard = co_await mtx.scoped_lock();
    while (!ready) {
        // Wait releases the held mutex and suspends the coroutine
        co_await cv.wait(mtx);
        // Re-acquire the lock before checking the condition again
        co_await mtx.lock();
    }

    // Or replace the loop with the predicate version, while holding the lock:
    // co_await cv.wait(mtx, [&]{ return ready; });
}

async<void> notifier() {
    {
        auto guard = co_await mtx.scoped_lock();
        ready = true;
    }
    // Wake up one waiting coroutine
    cv.notify_one();
    // Or wake up all waiting coroutines
    // cv.notify_all();
}
```

Waiters must stay alive until they are notified. Notifications that race with the waiter's unlock path are handled, but
the non-predicate `wait()` still returns without re-acquiring the user mutex.

### semaphore

A counting semaphore for controlling access to a shared resource with a limited number of permits.

```cpp
#include "coro/semaphore.hpp"

async<void> example() {
    // Create a semaphore with 3 permits
    coro::counting_semaphore sem(3);

    // Acquire a permit (suspends if not available)
    co_await sem.acquire();

    // Or acquire multiple permits
    // co_await sem.acquire(2);

    // Release a permit
    sem.release();

    // Or release multiple permits
    // sem.release(2);

    // Try to acquire without blocking
    if (sem.try_acquire()) {
        // Successfully acquired
        sem.release(); // Don't forget to release
    }

    // Check available permits
    int available = sem.available();

    // For binary semaphore (mutex-like behavior)
    // coro::binary_semaphore binary_sem(1);
}
```

`counting_semaphore(initial, max)` requires `0 <= initial <= max`. `acquire(n)` and `release(n)` require `n > 0`, and
`release(n)` must not raise the available permits above `max`. When coroutines are queued, permits are granted to the
front waiter first.

### channel

Go-style channel implementation for inter-coroutine communication:

#### Unbuffered Channel

```cpp
#include "coro/channel.hpp"
#include "coro/when.hpp"

async<void> producer(channel<int>& ch) {
    co_await ch.send(42);  // Blocks until receiver is ready
    co_await ch.send(100);
    ch.close();
}

async<void> consumer(channel<int>& ch) {
    while (true) {
        auto val = co_await ch.recv();
        if (!val.has_value()) {
            // Channel is closed
            break;
        }
        std::cout << "Received: " << *val << std::endl;
    }
}

async<void> example() {
    channel<int> ch;  // Unbuffered channel

    // Keep the channel alive until both tasks finish.
    co_await when_all(producer(ch), consumer(ch));
}
```

#### Buffered Channel

```cpp
async<void> example() {
    channel<int> ch(10);  // Buffer size of 10

    // Send doesn't block when buffer is not full
    co_await ch.send(1);
    co_await ch.send(2);

    // Check status
    bool empty = ch.empty();
    bool full = ch.full();
    size_t size = ch.size();
    size_t capacity = ch.capacity();
}
```

#### Channel Broadcast

The library also supports broadcast functionality, which sends a value to ALL waiting receivers simultaneously:

```cpp
#include <iostream>
#include "coro/channel.hpp"
#include "coro/executor_poll.hpp"

using namespace coro;

async<void> receive_once(channel<int>& ch, int id) {
    if (auto value = co_await ch.recv()) {
        std::cout << "Receiver " << id << " got: " << *value << std::endl;
    }
}

async<void> broadcast_once(channel<int>& ch) {
    size_t notified = co_await ch.broadcast(42);
    std::cout << "Broadcast notified " << notified << " receivers" << std::endl;
}

int main() {
    executor_poll exec;
    channel<int> ch;
    int finished = 0;
    auto on_finished = [&finished] {
        ++finished;
    };

    for (int id = 1; id <= 3; ++id) {
        receive_once(ch, id).detach_with_callback(exec, on_finished);
        exec.poll();  // Ensure this receiver reaches recv() before broadcasting.
    }

    broadcast_once(ch).detach_with_callback(exec, on_finished);
    while (finished != 4) {
        exec.poll();  // Keep the channel alive until all four tasks finish.
    }
    return 0;
}
```

Broadcast differs from regular send in that:

- `send()` sends to only one receiver (or blocks if no receiver is available)
- `broadcast()` sends to ALL currently waiting receivers simultaneously
- `broadcast()` returns the number of receivers that were notified
- If no receivers are waiting, `broadcast()` completes immediately without buffering

If copying a broadcast value throws, all receivers remain queued and no value is delivered; the caller can retry or close the channel.

### wait_group

A wait group, similar to Go's `sync.WaitGroup`, for coordinating multiple coroutines.

```cpp
#include "coro/wait_group.hpp"

async<void> worker_task(coro::wait_group& wg, std::string name, int work_ms) {
    // Do some work
    co_await sleep(work_ms * 1ms);
    std::cout << name << " completed\n";

    // Signal completion
    wg.done();  // or wg.add(-1);
}

async<void> example() {
    coro::wait_group wg;

    // Add 2 operations to wait for
    wg.add(2);

    // Launch worker coroutines
    spawn(executor, worker_task(wg, "Worker1", 100));
    spawn(executor, worker_task(wg, "Worker2", 150));

    // Wait for all operations to complete
    co_await wg.wait();
    // Or use direct co_await: co_await wg;

    // Check current count
    int count = wg.get_count();
}
```

The counter must never become negative. Destroying a wait group with outstanding work or waiters is invalid.

### latch

A countdown latch that allows coroutines to wait until a set number of operations complete.

```cpp
#include "coro/latch.hpp"

async<void> example() {
    // Create a latch with count 3
    coro::latch latch(3);

    // In some other coroutines, count down:
    // latch.count_down(); // Called 3 times by different coroutines

    // Wait for the latch to reach zero
    co_await latch.wait();
    // Or use direct co_await: co_await latch;

    // Alternative: count down and wait in one operation
    // co_await latch.arrive_and_wait();

    // Check current count
    int current_count = latch.get_count();
}
```

The initial count must be non-negative, and `count_down(n)` must not reduce the count below zero.

### event

An event synchronization primitive that allows one or more coroutines to wait until the event is set.

```cpp
#include "coro/event.hpp"

coro::event evt;

async<void> waiter() {
    // Wait for the event to be set
    co_await evt.wait();
    // Or use direct co_await: co_await evt;
}

async<void> setter() {
    // Set the event, waking up all waiters
    evt.set();

    // Clear the event (future waits will block until set() is called again)
    // evt.clear();

    // Check if the event is set (non-blocking)
    bool is_set = evt.is_set();
    co_return;
}
```

`event` is a manual-reset event: `set()` and `clear()` are idempotent state changes. `set()` releases all current waiters
and lets future waiters pass until `clear()` is called.
For suspended waiters associated with an executor, `set()` queues their continuation on that executor. A waiter without an executor resumes directly on the calling thread.

## Coroutine-Local Storage

Coroutine-local storage provides thread-local-like storage scoped to coroutines. Each `coro_local<T>` instance acts as a
unique key for storing values, similar to `thread_local` but for coroutines.

### Basic Usage

Enable `CORO_ENABLE_LOCAL_STORAGE` before including any coro header, consistently across all translation units.
With CMake, configure `-DCORO_ENABLE_LOCAL_STORAGE=ON` and link the `coro` target to propagate this definition.

```cpp
#ifndef CORO_ENABLE_LOCAL_STORAGE
#define CORO_ENABLE_LOCAL_STORAGE
#endif
#include <iostream>
#include <string>
#include "coro/coro_local.hpp"

// Define a storage key (like thread_local, typically static)
static coro::coro_local<int> request_id;
static coro::coro_local<std::string> user_name;

coro::async<void> process_request() {
    // Set value for current coroutine
    co_await request_id.set(42);
    co_await user_name.set("Alice");

    // Get value (returns default-constructed T if not set)
    int id = co_await request_id.get();
    std::string name = co_await user_name.get();

    std::cout << "Processing request " << id << " for " << name << std::endl;
}
```

### API Reference

```cpp
coro::coro_local<T> storage;

// Set value for current coroutine
co_await storage.set(value);

// Get value (returns default T{} if not set)
T value = co_await storage.get();

// Get as optional (returns std::nullopt if not set)
std::optional<T> opt = co_await storage.get_optional();

// Get a borrowed pointer (may point into an ancestor; nullptr if not set)
T* ptr = co_await storage.get_ptr();

// Check if value exists
bool exists = co_await storage.has();

// Erase the local override (an inherited value becomes visible again)
co_await storage.erase();
```

### Inheritance from Parent Coroutine

Directly awaited child coroutines automatically inherit access to parent values. Calling `set()` creates or replaces a local
value without changing the parent. Detached tasks start with independent storage.
`inherit_coro_local()` only ensures the current coroutine has a storage map; it is optional for inheritance and local writes.

```cpp
static coro::coro_local<int> context_id;

async<void> parent_coro() {
    co_await context_id.set(100);

    // Child coroutine can read parent's value
    auto child = []() -> async<void> {
        int id = co_await context_id.get();
        std::cout << "Child sees: " << id << std::endl;  // Prints: 100
        co_return;
    };

    co_await child();
}
```

### Local Overrides

Use `set()` to override an inherited value in the child. `erase()` removes only that local override, exposing the inherited
value again. `get_ptr()` returns a borrowed pointer to the stored value, which may belong to an ancestor; modifying through
that pointer changes the ancestor's value. To keep a modification local, read with `get()` and write back with `set()`:

```cpp
async<void> parent_coro() {
    co_await context_id.set(100);

    auto child = []() -> async<void> {
        // Read parent's value
        int id = co_await context_id.get();  // 100

        // Modify locally (doesn't affect parent)
        co_await context_id.set(200);

        id = co_await context_id.get();  // 200
        co_return;
    };

    co_await child();

    // Parent's value is unchanged
    int id = co_await context_id.get();  // Still 100
}
```

### Isolation Between Concurrent Coroutines

Each coroutine has its own isolated storage. Concurrent coroutines do not interfere with each other:

```cpp
async<void> concurrent_example() {
    static coro::coro_local<int> task_id;

    auto task_a = []() -> async<void> {
        co_await task_id.set(111);
        co_await coro::sleep(50ms);
        int id = co_await task_id.get();  // Still 111
        co_return;
    };

    auto task_b = []() -> async<void> {
        co_await task_id.set(222);
        co_await coro::sleep(50ms);
        int id = co_await task_id.get();  // Still 222
        co_return;
    };

    // Both tasks run concurrently with isolated storage
    co_await coro::spawn_local(task_a());
    co_await coro::spawn_local(task_b());
}
```

## Callback to Coroutine

Use `callback_awaiter` to convert callback-style APIs to coroutines:

The awaiting coroutine must have a bound or inherited executor. The two overloads differ in whether the registration function receives that executor.

With exceptions enabled, failures while storing or extracting the callback result are rethrown at `co_await`.

```cpp
// Registration without an executor parameter
async<int> async_operation() {
    int result = co_await callback_awaiter<int>([](auto callback) {
        // Async operation, call callback when done
        std::thread([callback = std::move(callback)]() {
            std::this_thread::sleep_for(100ms);
            callback(42);  // Return result
        }).detach();
    });
    co_return result;
}

// Registration with an executor parameter
async<void> async_void_operation() {
    co_await callback_awaiter<void>([](executor* exec, auto callback) {
        // Can use executor for scheduling
        exec->post_delayed_ns(std::move(callback), 1000000);  // Execute after 1ms
    });
}
```

## ASIO Adaptor

The library provides an adaptor for interoperability between `coro::async<T>` and `asio::awaitable<T>`, allowing you to
mix coro and ASIO coroutines seamlessly.

Both adaptors support move-only results without requiring default construction or assignment.

Unbound coro tasks awaited from an ASIO strand inherit that strand, including nested calls through `await_asio()`.

### asio_executor

`asio_executor` adapts `asio::io_context` to the `coro::executor` interface, enabling coro coroutines to run on an ASIO
event loop:

```cpp
#include "coro/adaptor/asio_adaptor.hpp"

asio::io_context io_ctx;
coro::asio_executor exec(io_ctx);

// Launch coro coroutines on ASIO event loop
spawn(exec, my_coro_task());

// Run the ASIO event loop
io_ctx.run();
```

### await_asio

`await_asio()` allows coro coroutines to `co_await` ASIO awaitables. The awaiting coro task must be bound to or inherit an
`asio_executor`:

Stopping the `io_context` pauses queued continuations. After `restart()` and another `run()`, the awaiting coro task can continue.
Keep the executor and the task's referenced data alive until the task finishes.

```cpp
#include "coro/adaptor/asio_adaptor.hpp"

// ASIO coroutine
asio::awaitable<int> asio_fetch_data() {
    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    timer.expires_after(100ms);
    co_await timer.async_wait(asio::use_awaitable);
    co_return 42;
}

// coro coroutine calling ASIO coroutine
coro::async<void> coro_task() {
    // Use await_asio to call ASIO awaitable from coro
    int result = co_await coro::await_asio(asio_fetch_data());
    std::cout << "Got: " << result << std::endl;
    
    // Also works with void return type
    co_await coro::await_asio(some_asio_void_task());
}
```

### await_coro

`await_coro()` allows ASIO coroutines to `co_await` coro asyncs. The ASIO coroutine must run on an `asio::io_context`
executor or a strand backed by that context; other contexts such as `asio::thread_pool` are not supported:

Cancelling the ASIO wait reports `asio::error::operation_aborted` if the coro task has not completed.
The detached coro task continues running, so its referenced data must remain alive until it finishes.

```cpp
#include "coro/adaptor/asio_adaptor.hpp"

// coro coroutine
coro::async<int> coro_compute() {
    co_await coro::sleep(100ms);
    co_return 99;
}

// ASIO coroutine calling coro coroutine
asio::awaitable<void> asio_task() {
    // Use await_coro to call coro async from ASIO
    int result = co_await coro::await_coro(coro_compute());
    std::cout << "Got: " << result << std::endl;
    
    // Also works with void return type
    co_await coro::await_coro(some_coro_void_task());
}
```

### Nested Calls

You can have deeply nested cross-framework calls:

```cpp
// coro -> asio -> coro
coro::async<int> coro_task() {
    // Call ASIO coroutine which internally calls coro coroutine
    int result = co_await coro::await_asio(asio_intermediate());
    co_return result;
}

asio::awaitable<int> asio_intermediate() {
    // Call coro coroutine from within ASIO
    int value = co_await coro::await_coro(another_coro_task());
    co_return value * 2;
}

// asio -> coro -> asio  
asio::awaitable<int> asio_task() {
    int result = co_await coro::await_coro(coro_intermediate());
    co_return result;
}

coro::async<int> coro_intermediate() {
    int value = co_await coro::await_asio(another_asio_task());
    co_return value + 1;
}
```

### Exception Handling

Exceptions propagate correctly across framework boundaries:

```cpp
// Exception from ASIO caught in coro
coro::async<void> coro_catches_asio_exception() {
    try {
        co_await coro::await_asio(asio_throws());
    } catch (const std::exception& e) {
        std::cout << "Caught: " << e.what() << std::endl;
    }
}

// Exception from coro caught in ASIO
asio::awaitable<void> asio_catches_coro_exception() {
    try {
        co_await coro::await_coro(coro_throws());
    } catch (const std::exception& e) {
        std::cout << "Caught: " << e.what() << std::endl;
    }
}
```

### Complete Example

```cpp
#include <chrono>
#include <iostream>
#include "coro/coro.hpp"
#include "coro/time.hpp"
#include "coro/adaptor/asio_adaptor.hpp"

using namespace coro;

asio::awaitable<void> asio_sleep(std::chrono::milliseconds duration) {
    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    timer.expires_after(duration);
    co_await timer.async_wait(asio::use_awaitable);
}

asio::awaitable<int> asio_compute() {
    co_return 42;
}

async<void> main_task() {
    // Mix coro and ASIO coroutines freely
    co_await sleep(50ms);  // coro timer
    co_await await_asio(asio_sleep(50ms));  // ASIO timer
    
    int result = co_await await_asio(asio_compute());
    std::cout << "Result: " << result << std::endl;
}

int main() {
    asio::io_context io_ctx;
    asio_executor exec(io_ctx);
    
    spawn(exec, main_task());
    
    io_ctx.run();
    return 0;
}
```

## Configuration Options

Define configuration and debug macros before including any coro header, with the same definitions in every translation unit that uses the library. Prefer a shared configuration header or target-wide build definitions. Mixing settings such as `CORO_DISABLE_EXCEPTION` can change promise layouts and violates the one-definition rule, even if a debug build appears to work.

### Disable Exceptions

Define `CORO_DISABLE_EXCEPTION` macro to disable exception support and reduce overhead:

```cpp
#define CORO_DISABLE_EXCEPTION
#include "coro/coro.hpp"
```

Or via CMake:

```cmake
add_definitions(-DCORO_DISABLE_EXCEPTION)
```

### Debug Coroutine Leaks

Logging hooks run on the calling thread and may be invoked concurrently; use a thread-safe logger. Leak counts are snapshots taken when each event is recorded.

The debug tracker retains its shared registry until process exit so static constructors and destructors can safely create coroutines.

Leak tracking preserves aligned allocations requested by the compiler. Clang builds with over-aligned coroutine state may require `-fcoro-aligned-allocation`.

```cpp
#define CORO_DEBUG_PROMISE_LEAK
#define CORO_DEBUG_LEAK_LOG printf  // Or other log function
#include "coro/coro.hpp"

// Check at program end
debug_coro_promise::dump();
```

### Debug Coroutine Lifecycle

```cpp
#define CORO_DEBUG_LIFECYCLE printf  // Or other log function
#include "coro/coro.hpp"
```

### Async Stacks in GDB / LLDB

Enable optional metadata when building your application (off by default):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCORO_DEBUG_ASYNC_STACK=ON
cmake --build build -j
```

For builds outside CMake, define `CORO_DEBUG_ASYNC_STACK` before including any coro headers
and compile with debug information (`-g`, or `/Zi` on MSVC). Use the **same macro setting in
all translation units**: this option changes the promise layout. CMake propagates it through
the `coro` interface target. It does not require `CORO_DEBUG_PROMISE_LEAK`.

Use a debugger with Python 3 scripting enabled. Load the script, stop the process, then list
frames and select an ID or frame address:

```text
(gdb) source tools/debugger/coro_debug.py
(gdb) coro-list
(gdb) coro-bt id:4
(gdb) coro-bt 0x12345678

(lldb) command script import tools/debugger/coro_debug.py
(lldb) coro-list
(lldb) coro-bt id:4
```

Example output (addresses and IDs vary):

```text
#0 id:4 frame=0x12345678 [live] async<int> debug_leaf() at test/coro_debug.cpp:28
#1 id:3 frame=0x12345000 [live] async<int> debug_middle() at test/coro_debug.cpp:33
#2 id:2 frame=0x12344000 [live] async<void> debug_root(int &) at test/coro_debug.cpp:37
```

The script follows `co_await` relationships, including internal `when_all` / `when_any`
wrappers, across thread switches. Each promise has an intrusive diagnostic record with a
unique lifetime ID. Records are removed when the frame is destroyed; reused addresses cannot
silently become different parents. A `when_any` loser can outlive its parent: the script reports
that the parent is no longer live. Ordinary `spawn` / `detach` tasks start independent chains;
there is no implicit link to the code that launched them. External promises end the chain
with an explicit metadata-unavailable message.

These are **logical async stacks**, complementary to native `bt`. Locations identify coroutine
functions, not the current instruction or every suspension point. `[live]` means the frame has
not reached final suspend; it does not distinguish running, suspended, and not yet started.
`[completed]` frames remain listed until destroyed, with their parent link cleared.
When the standard library lacks `std::source_location`, or with GCC before version 14,
compiler builtins collect source locations instead. Function names in this configuration
omit the full type signature.

Both commands read memory without calling functions in the debugged process, acquiring its
locks, or assuming a compiler's coroutine frame ABI. Stop **all threads** before using them.
They also work with a core containing the registry, frame metadata, and referenced strings,
plus matching debug symbols. LLDB can read omitted source strings from the matching binary
when a compact core leaves out read-only image pages. A regular Breakpad minidump may omit
the required frame memory; this is not a
Breakpad stackwalker extension. Interrupted registry updates, corrupt links, missing memory,
and traversal limits are reported rather than followed indefinitely (4096 records, 256 frames).

When enabled, metadata adds per-promise storage and a global mutex acquisition for registration,
parent changes, completion, and destruction; it performs no extra heap allocation per record.
When disabled, neither the record nor its synchronization is compiled in. Optimized builds
still need matching debug information; validate your compiler/debugger combination before
relying on it for production dumps.

To run the regression fixture and debugger checks:

```bash
./build/coro_debug
python3 test/debugger_test.py
python3 test/debugger_smoke.py gdb build/coro_debug
python3 test/debugger_smoke.py lldb build/coro_debug --core
```

## Building Tests

```bash
mkdir build && cd build
cmake ..
make

# Run tests
./coro_task
./coro_mutex
./coro_channel
./coro_when
./coro_condition_variable
./coro_event
./coro_latch
./coro_semaphore
./coro_wait_group
```

To enable the ASIO adaptor test, configure with the `ASIO_PATH` environment variable pointing to the
include directory containing `asio.hpp`. The build directory remembers this path for subsequent
reconfiguration. Set `ASIO_PATH` to a new path when configuring to change it, or explicitly set it to
an empty value to disable the ASIO test. Disabling exceptions still selects the placeholder test.

### CMake Options

| Option                         | Default              | Description               |
|--------------------------------|----------------------|---------------------------|
| `CORO_BUILD_TEST`              | ON (as main project) | Build tests               |
| `CORO_TEST_RUNNER_VERY_SLOW`   | OFF                  | Relax test timing for slow runners |
| `CORO_ENABLE_SANITIZE_ADDRESS` | OFF                  | Enable AddressSanitizer   |
| `CORO_ENABLE_SANITIZE_THREAD`  | OFF                  | Enable ThreadSanitizer    |
| `CORO_DISABLE_EXCEPTION`       | OFF                  | Disable exception support |
| `CORO_DEBUG_ASYNC_STACK`       | OFF                  | Enable async debugger metadata |

`CORO_TEST_RUNNER_VERY_SLOW` is automatically enabled on macOS GitHub Actions runners.

## Project Structure

```
coro/
├── include/
│   ├── coro.hpp              # Main header (includes all components)
│   └── coro/
│       ├── coro.hpp          # Core coroutine implementation
│       ├── executor.hpp      # Executor interface
│       ├── executor_base.hpp  # Basic task executor
│       ├── executor_poll.hpp # Polling executor
│       ├── executor_loop.hpp # Event loop executor
│       ├── time.hpp          # Timer
│       ├── channel.hpp       # Channel
│       ├── condition_variable.hpp # Condition variable
│       ├── event.hpp         # Event synchronization primitive
│       ├── latch.hpp         # Latch
│       ├── mutex.hpp         # Mutex
│       ├── semaphore.hpp     # Semaphore
│       ├── wait_group.hpp    # Wait group
│       ├── when.hpp          # when_all/when_any
│       └── adaptor/
│           └── asio_adaptor.hpp  # ASIO interoperability adaptor
└── test/                     # Test files
```
