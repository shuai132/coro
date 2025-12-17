# coro

[![CI](https://github.com/shuai132/coro/actions/workflows/ci.yml/badge.svg)](https://github.com/shuai132/coro/actions/workflows/ci.yml)

A lightweight C++20 coroutine library with async tasks, concurrency control, and synchronization primitives.

[中文文档](README_CN.md)

## Table of Contents

- [API Overview](#api-overview)
- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Executors](#executors)
- [Timer](#timer)
- [Concurrency Operations](#concurrency-operations)
- [Channel](#channel)
- [Mutex](#mutex)
- [Callback to Coroutine](#callback-to-coroutine)
- [Configuration Options](#configuration-options)
- [Building Tests](#building-tests)
- [Project Structure](#project-structure)

## API Overview

| Name                                         | Description                                          |
|----------------------------------------------|------------------------------------------------------|
| `coro::async<T>`                             | Async task type, supports `co_await` and `co_return` |
| `coro::co_spawn(executor, awaitable)`        | Spawn a coroutine on an executor                     |
| `coro::when_all(awaitables...) -> awaitable` | Wait for all tasks to complete                       |
| `coro::when_any(awaitables...) -> awaitable` | Wait for any task to complete                        |
| `coro::sleep(duration)`                      | Async wait for specified duration (chrono duration)  |
| `coro::delay(ms)`                            | Async wait for specified milliseconds                |
| `coro::mutex`                                | Coroutine-safe mutex                                 |
| `coro::channel<T>`                           | Go-style channel for inter-coroutine communication   |
| `coro::executor`                             | Executor base class interface                        |
| `coro::executor_loop`                        | Event loop based executor                            |
| `coro::executor_poll`                        | Polling based executor                               |
| `coro::current_executor()`                   | Get current executor                                 |
| `coro::callback_awaiter<T>`                  | Convert callback-style APIs to coroutines            |

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

## Requirements

- C++20 compatible compiler (GCC 10+, Clang 10+, MSVC 19.28+)
- CMake 3.15+ (optional, for building tests)

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

```cpp
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
    
    // Launch coroutine
    co_spawn(executor, process());
    // Or: process().detach(executor);
    
    // Run event loop
    executor.run_loop();
    return 0;
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

## Executors

Two executor implementations are provided:

### executor_loop

Condition variable based event loop, suitable for main thread:

```cpp
#include "coro/executor_loop.hpp"

executor_loop executor;

// Launch coroutines...

// Block until stop() is called
executor.run_loop();
```

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

## Concurrency Operations

### when_all

Wait for all tasks to complete:

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

## Channel

Go-style channel implementation for inter-coroutine communication:

### Unbuffered Channel

```cpp
#include "coro/channel.hpp"

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
    
    auto& exec = *co_await current_executor();
    co_spawn(exec, producer(ch));
    co_spawn(exec, consumer(ch));
}
```

### Buffered Channel

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

## Callback to Coroutine

Use `callback_awaiter` to convert callback-style APIs to coroutines:

```cpp
// Basic usage (without executor)
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

// Version with executor
async<void> async_void_operation() {
    co_await callback_awaiter<void>([](executor* exec, auto callback) {
        // Can use executor for scheduling
        exec->post_delayed_ns(std::move(callback), 1000000);  // Execute after 1ms
    });
}
```

## Configuration Options

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
```

### CMake Options

| Option                         | Default              | Description               |
|--------------------------------|----------------------|---------------------------|
| `CORO_BUILD_TEST`              | ON (as main project) | Build tests               |
| `CORO_ENABLE_SANITIZE_ADDRESS` | OFF                  | Enable AddressSanitizer   |
| `CORO_ENABLE_SANITIZE_THREAD`  | OFF                  | Enable ThreadSanitizer    |
| `CORO_DISABLE_EXCEPTION`       | OFF                  | Disable exception support |

## Project Structure

```
coro/
├── include/
│   ├── coro.hpp              # Main header (includes all components)
│   └── coro/
│       ├── coro.hpp          # Core coroutine implementation
│       ├── executor.hpp      # Executor interface
│       ├── executor_basic_task.hpp  # Basic task executor
│       ├── executor_poll.hpp # Polling executor
│       ├── executor_loop.hpp # Event loop executor
│       ├── time.hpp          # Timer
│       ├── channel.hpp       # Channel
│       ├── mutex.hpp         # Mutex
│       └── when.hpp          # when_all/when_any
└── test/                     # Test files
```
