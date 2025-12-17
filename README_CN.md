# coro

[![CI](https://github.com/shuai132/coro/actions/workflows/ci.yml/badge.svg)](https://github.com/shuai132/coro/actions/workflows/ci.yml)

一个轻量级的 C++20 协程库，支持异步任务、并发控制和同步原语。

[English](README.md)

## 目录

- [API 概览](#api-概览)
- [特性](#特性)
- [要求](#要求)
- [安装](#安装)
- [快速开始](#快速开始)
- [执行器 (Executor)](#执行器-executor)
- [定时器](#定时器)
- [并发操作](#并发操作)
- [Channel](#channel)
- [Mutex](#mutex)
- [回调转协程](#回调转协程)
- [配置选项](#配置选项)
- [构建测试](#构建测试)
- [项目结构](#项目结构)

## API 概览

| 名称                                           | 说明                                 |
|----------------------------------------------|------------------------------------|
| `coro::async<T>`                             | 异步任务类型，支持 `co_await` 和 `co_return` |
| `coro::co_spawn(executor, awaitable)`        | 在执行器上启动协程                          |
| `coro::when_all(awaitables...) -> awaitable` | 等待所有任务完成                           |
| `coro::when_any(awaitables...) -> awaitable` | 等待任意一个任务完成                         |
| `coro::sleep(duration)`                      | 异步等待指定时间（chrono duration）          |
| `coro::delay(ms)`                            | 异步等待指定毫秒数                          |
| `coro::mutex`                                | 协程安全的互斥锁                           |
| `coro::channel<T>`                           | Go 风格的 channel，用于协程间通信             |
| `coro::executor`                             | 执行器基类接口                            |
| `coro::executor_loop`                        | 基于事件循环的执行器                         |
| `coro::executor_poll`                        | 基于轮询的执行器                           |
| `coro::current_executor()`                   | 获取当前执行器                            |
| `coro::callback_awaiter<T>`                  | 将回调式 API 转换为协程                     |

## 特性

- 🚀 **纯头文件库**：无需编译，直接包含使用
- 📦 **C++20 标准**：基于 C++20 协程特性实现
- 🔄 **异步任务 (async/awaitable)**：支持 `co_await` 和 `co_return`
- ⏰ **定时器支持**：内置 `sleep` 和 `delay` 异步等待
- 🔀 **并发原语**：支持 `when_all` 和 `when_any` 并发操作
- 📨 **Channel**：Go 风格的 channel，支持缓冲和无缓冲模式
- 🔒 **Mutex**：协程安全的互斥锁，支持 RAII 风格的 `scoped_lock`
- 🎛️ **执行器**：提供轮询模式 (`executor_poll`) 和事件循环模式 (`executor_loop`) 或自定义实现
- ⚠️ **异常支持**：可选的异常处理，支持通过宏禁用
- 🛠️ **调试支持**：内置协程泄漏检测功能
- 🔍 **单元测试**：完善的单元测试和集成测试
- 📦 **嵌入式支持**：支持 MCU 和嵌入式平台

## 要求

- C++20 兼容的编译器（GCC 10+、Clang 10+、MSVC 19.28+）
- CMake 3.15+（可选，用于构建测试）

## 安装

### 方式一：直接包含

由于是纯头文件库，直接将 `include` 目录添加到项目的包含路径即可：

```cpp
#include "coro.hpp"
```

### 方式二：CMake

```cmake
add_subdirectory(coro)
target_link_libraries(your_target coro)
```

## 快速开始

### 基本用法

```cpp
#include "coro/coro.hpp"
#include "coro/time.hpp"
#include "coro/executor_loop.hpp"

using namespace coro;

// 定义一个返回 int 的异步任务
async<int> fetch_data() {
    co_await sleep(100ms);  // 异步等待 100 毫秒
    co_return 42;
}

// 定义一个 void 类型的异步任务
async<void> process() {
    int data = co_await fetch_data();
    std::cout << "Data: " << data << std::endl;
}

int main() {
    executor_loop executor;
    
    // 启动协程
    co_spawn(executor, process());
    // 或者: process().detach(executor);
    
    // 运行事件循环
    executor.run_loop();
    return 0;
}
```

### 使用回调启动协程

```cpp
async<int> compute() {
    co_await sleep(50ms);
    co_return 123;
}

// 使用回调处理结果
compute().detach_with_callback(
    executor,
    [](int result) {
        std::cout << "Result: " << result << std::endl;
    },
    [](std::exception_ptr ex) {
        // 可选的异常处理
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }
);
```

### 获取当前执行器

```cpp
async<void> example() {
    executor* exec = co_await current_executor();
    // 使用 exec...
}
```

## 执行器 (Executor)

目前提供两种执行器实现：

### executor_loop

基于条件变量的事件循环，适合作为主线程运行：

```cpp
#include "coro/executor_loop.hpp"

executor_loop executor;

// 启动协程...

// 阻塞运行直到 stop() 被调用
executor.run_loop();
```

### executor_poll

非阻塞轮询模式，适合集成到现有事件循环：

```cpp
#include "coro/executor_poll.hpp"

executor_poll executor;

// 启动协程...

// 在你的主循环中调用
while (!executor.stopped()) {
    executor.poll();
    // 其他工作...
    std::this_thread::sleep_for(10ms);
}
```

### 自定义执行器

继承 `coro::executor` 接口实现自定义执行器：

```cpp
struct my_executor : coro::executor {
    void dispatch(std::function<void()> fn) override;      // 立即或稍后执行
    void post(std::function<void()> fn) override;          // 稍后执行
    void post_delayed_ns(std::function<void()> fn, uint64_t delay_ns) override;  // 延迟执行
    void stop() override;                                  // 停止执行器
};
```

## 定时器

```cpp
#include "coro/time.hpp"

async<void> timer_example() {
    // 使用 chrono duration
    co_await sleep(100ms);
    co_await sleep(std::chrono::seconds(1));
    
    // 或使用毫秒延迟
    co_await delay(500);  // 500 毫秒
}
```

## 并发操作

### when_all

等待所有任务完成：

```cpp
#include "coro/when.hpp"

async<int> task1() { co_await sleep(100ms); co_return 1; }
async<int> task2() { co_await sleep(50ms);  co_return 2; }
async<void> task3() { co_await sleep(75ms); }

async<void> example() {
    // 等待所有任务完成，返回非 void 结果的 tuple
    auto [r1, r2] = co_await when_all(task1(), task2(), task3());
    // r1 = 1, r2 = 2
    // task3 是 void 类型，不包含在结果中
    
    // 如果所有任务都是 void 类型
    co_await when_all(task3(), task3());
    
    // 如果只有一个非 void 任务，直接返回值（不是 tuple）
    int result = co_await when_all(task3(), task1(), task3());
    // result = 1
}
```

### when_any

等待任意一个任务完成：

```cpp
async<void> example() {
    // 返回第一个完成的任务
    auto result = co_await when_any(task1(), task2(), task3());
    
    // result.index 表示完成的任务索引
    std::cout << "Task " << result.index << " completed first" << std::endl;
    
    // 获取完成任务的返回值（如果不是 void）
    if (result.index == 0) {
        int value = result.template get<0>();
    } else if (result.index == 1) {
        int value = result.template get<1>();
    }
    // index == 2 的 task3 是 void 类型
}
```

## Channel

Go 风格的 channel 实现，用于协程间通信：

### 无缓冲 Channel

```cpp
#include "coro/channel.hpp"

async<void> producer(channel<int>& ch) {
    co_await ch.send(42);  // 阻塞直到有接收者
    co_await ch.send(100);
    ch.close();
}

async<void> consumer(channel<int>& ch) {
    while (true) {
        auto val = co_await ch.recv();
        if (!val.has_value()) {
            // Channel 已关闭
            break;
        }
        std::cout << "Received: " << *val << std::endl;
    }
}

async<void> example() {
    channel<int> ch;  // 无缓冲 channel
    
    auto& exec = *co_await current_executor();
    co_spawn(exec, producer(ch));
    co_spawn(exec, consumer(ch));
}
```

### 缓冲 Channel

```cpp
async<void> example() {
    channel<int> ch(10);  // 缓冲大小为 10
    
    // 缓冲未满时 send 不会阻塞
    co_await ch.send(1);
    co_await ch.send(2);
    
    // 检查状态
    bool empty = ch.empty();
    bool full = ch.full();
    size_t size = ch.size();
    size_t capacity = ch.capacity();
}
```

## Mutex

协程安全的互斥锁：

### 使用 scoped_lock（推荐）

```cpp
#include "coro/mutex.hpp"

coro::mutex mtx;

async<void> critical_section() {
    {
        auto guard = co_await mtx.scoped_lock();
        // 临界区代码
        // ...
    }  // 自动解锁
}
```

### 手动 lock/unlock

```cpp
async<void> manual_lock() {
    co_await mtx.lock();
    // 临界区代码
    mtx.unlock();
}
```

### 提前解锁

```cpp
async<void> early_unlock() {
    auto guard = co_await mtx.scoped_lock();
    // 临界区代码...
    
    guard.unlock();  // 提前手动解锁
    
    // 非临界区代码...
}
```

## 回调转协程

使用 `callback_awaiter` 将回调式 API 转换为协程：

```cpp
// 基本用法（无执行器）
async<int> async_operation() {
    int result = co_await callback_awaiter<int>([](auto callback) {
        // 异步操作，完成后调用 callback
        std::thread([callback = std::move(callback)]() {
            std::this_thread::sleep_for(100ms);
            callback(42);  // 返回结果
        }).detach();
    });
    co_return result;
}

// 需要执行器的版本
async<void> async_void_operation() {
    co_await callback_awaiter<void>([](executor* exec, auto callback) {
        // 可以使用执行器进行调度
        exec->post_delayed_ns(std::move(callback), 1000000);  // 1ms 后执行
    });
}
```

## 配置选项

### 禁用异常

定义 `CORO_DISABLE_EXCEPTION` 宏可以禁用异常支持，减少开销：

```cpp
#define CORO_DISABLE_EXCEPTION
#include "coro/coro.hpp"
```

或通过 CMake：

```cmake
add_definitions(-DCORO_DISABLE_EXCEPTION)
```

### 调试协程泄漏

```cpp
#define CORO_DEBUG_PROMISE_LEAK
#define CORO_DEBUG_LEAK_LOG printf  // 或其他日志函数
#include "coro/coro.hpp"

// 在程序结束时检查
debug_coro_promise::dump();
```

### 调试协程生命周期

```cpp
#define CORO_DEBUG_LIFECYCLE printf  // 或其他日志函数
#include "coro/coro.hpp"
```

## 构建测试

```bash
mkdir build && cd build
cmake ..
make

# 运行测试
./coro_task
./coro_mutex
./coro_channel
./coro_when
```

### CMake 选项

| 选项                             | 默认值         | 说明                  |
|--------------------------------|-------------|---------------------|
| `CORO_BUILD_TEST`              | ON (当作为主项目) | 构建测试                |
| `CORO_ENABLE_SANITIZE_ADDRESS` | OFF         | 启用 AddressSanitizer |
| `CORO_ENABLE_SANITIZE_THREAD`  | OFF         | 启用 ThreadSanitizer  |
| `CORO_DISABLE_EXCEPTION`       | OFF         | 禁用异常支持              |

## 项目结构

```
coro/
├── include/
│   ├── coro.hpp              # 主头文件（包含所有组件）
│   └── coro/
│       ├── coro.hpp          # 核心协程实现
│       ├── executor.hpp      # 执行器接口
│       ├── executor_basic_task.hpp  # 基础任务执行器
│       ├── executor_poll.hpp # 轮询执行器
│       ├── executor_loop.hpp # 事件循环执行器
│       ├── time.hpp          # 定时器
│       ├── channel.hpp       # Channel
│       ├── mutex.hpp         # Mutex
│       └── when.hpp          # when_all/when_any
└── test/                     # 测试文件
```
