# coro

[![CI](https://github.com/shuai132/coro/actions/workflows/ci.yml/badge.svg)](https://github.com/shuai132/coro/actions/workflows/ci.yml)
<img alt="feature" src="https://img.shields.io/badge/c++20-Coroutines-orange">
<img alt="language" src="https://img.shields.io/github/languages/top/shuai132/coro">

一个轻量级的 C++20 协程库，支持异步任务、并发控制和同步原语。

[English](README.md)

## 目录

- [前言](#前言)
- [API 概览](#api-概览)
- [特性](#特性)
- [要求](#要求)
- [安装](#安装)
- [快速开始](#快速开始)
- [执行器 (Executor)](#执行器-executor)
- [定时器](#定时器)
- [并发操作](#并发操作)
- [同步原语 (Synchronization Primitives)](#同步原语)
    - [mutex](#mutex)
    - [condition_variable](#condition_variable)
    - [semaphore](#semaphore)
    - [channel](#channel)
    - [wait_group](#wait_group)
    - [latch](#latch)
    - [event](#event)
- [协程本地存储](#协程本地存储)
- [回调转协程](#回调转协程)
- [ASIO 适配器](#asio-适配器)
- [配置选项](#配置选项)
- [构建测试](#构建测试)
- [项目结构](#项目结构)

## 前言

本项目最初是为了学习C++20协程而写，周末有空就索性完善了一些必要的API和同步原语，功能已非常完备。

### 设计目标：

* 流程清晰、简单易懂（希望如此）

  C++20的协程设计非常晦涩，其api主要面向库开发者。而对于任何的协程库，想要读懂其中的设计，还是要先理解协程api的流程和行为。
  也因此未必能简单易懂，只是相对而言，本库尽量不使用晦涩的模板、concept约束、类型嵌套、切换跳转行为。


* 多平台支持

  嵌入式平台支持，甚至可以用在MCU，这也是设计出发点之一。在一些平台上没有通用OS，也没有RTOS，甚至不支持异常，因此很多开源库无法使用。
  而且对编译器要求较高，一些复杂的特性，在一些编译器无法完全支持，即使gcc版本看起来比较高。


* bugfree（尤其是内存和线程问题）

  我发现很多开源库，竟然单元测试都特别简单，尤其是在多线程方面几乎都未测试。也缺少线程竞争和内存泄露的自动化测试（基于Sanitize），其高质量太依赖使用者反馈，即使知名度很高，但是实际上工程化是不够好的。

也有了解到已经有一些知名的C++20协程的开源库，比如：

* [https://github.com/alibaba/async_simple](https://github.com/alibaba/async_simple)
* [https://github.com/jbaldwin/libcoro](https://github.com/jbaldwin/libcoro)

### 为什么重复造轮子：

* 首先是设计目标不同，上面已经有提到。


* 另一个很大的原因是，设计取舍不同。我想要把**易于理解和使用**放在第一位，无论是api设计、功能设计和实现。

  比如`libcoro`里支持`co_await tp->schedule()`而且作为切换线程的推荐范式，我认为这是极其不恰当的。在同一个代码块上下文切换线程非常反直觉和容易出错。

  再比如`async_simple`和`libcoro`的同步原语设计，需要用户在协程上下文调用，比如`co_await semaphore.release()`。
  我认为宽松的约束更容易使用，用户可以在任意地方调用`semaphore.release()`。

  有很多类似的设计取舍，不再一一列举。


* 它们在协程类型和行为的设计上，稍显繁琐。

  比如为了detach一个协程，要经过层层包装。这虽然不是一个大的问题，但我认为是完全没必要的。这会经过好多个协程创建到销毁的生命周期，难以排查问题。


* 总结

  这些开源库都有自己独到的设计。后来看到`async_simple`的实现后，惊奇的发现有很多设计都很类似！但是细节和取舍又有所不同。

## API 概览

| 名称                                           | 说明                                 |
|----------------------------------------------|------------------------------------|
| `coro::async<T>`                             | 异步任务类型，支持 `co_await` 和 `co_return` |
| `coro::spawn(executor, awaitable)`           | 在执行器上启动协程                          |
| `coro::spawn_local(awaitable)`               | 在当前执行器上启动协程                        |
| `coro::when_all(awaitables...) -> awaitable` | 等待所有任务完成                           |
| `coro::when_any(awaitables...) -> awaitable` | 等待任意一个任务完成                         |
| `coro::sleep(duration)`                      | 异步等待指定时间（chrono duration）          |
| `coro::delay(ms)`                            | 异步等待指定毫秒数                          |
| `co_await chrono_duration`                   | 直接异步等待（例如`co_await 1s`）            |
| `coro::mutex`                                | 协程安全的互斥锁                           |
| `coro::condition_variable`                   | 协程安全的条件变量，用于同步操作                   |
| `coro::event`                                | 事件同步原语                             |
| `coro::latch`                                | 倒计时门闩，用于同步操作                       |
| `coro::semaphore`                            | 计数信号量，用于资源控制                       |
| `coro::wait_group`                           | 等待组，用于协调多个协程                       |
| `coro::channel<T>`                           | Go 风格的 channel，用于协程间通信             |
| `coro::executor`                             | 执行器基类接口                            |
| `coro::executor_loop`                        | 基于事件循环的执行器                         |
| `coro::executor_poll`                        | 基于轮询的执行器                           |
| `coro::current_executor()`                   | 获取当前执行器                            |
| `coro::callback_awaiter<T>`                  | 将回调式 API 转换为协程                     |
| `coro::coro_local<T>`                        | 协程本地存储（类似 thread_local）            |
| `coro::inherit_coro_local()`                 | 继承父协程的本地存储                         |

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
- 🧩 **扩展同步原语**：提供额外的同步工具，包括条件变量、事件、门闩、信号量和等待组
- 🗂️ **协程本地存储**：类似 thread_local 的协程作用域存储，支持继承

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

    // 在指定执行器上启动协程
    spawn(executor, process());
    // 或者: process().detach(executor);

    // 运行事件循环
    executor.run_loop();
    return 0;
}

// 在协程内部使用 spawn_local 在当前执行器上启动协程
async<void> main_task() {
    co_await spawn_local(process());
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

## 同步原语

该库提供了多个协程安全的同步原语：

### mutex

协程安全的互斥锁：

#### 使用 scoped_lock（推荐）

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

互斥锁销毁前必须处于未加锁状态，等待中的协程也必须一直存活到被恢复。

#### 手动 lock/unlock

```cpp
async<void> manual_lock() {
    co_await mtx.lock();
    // 临界区代码
    mtx.unlock();
}
```

#### 提前解锁

```cpp
async<void> early_unlock() {
    auto guard = co_await mtx.scoped_lock();
    // 临界区代码...

    guard.unlock();  // 提前手动解锁

    // 非临界区代码...
}
```

### condition_variable

协程安全的条件变量，类似于 Go 的 `sync.Cond`。必须与 `coro::mutex` 一起使用。

```cpp
#include "coro/condition_variable.hpp"
#include "coro/mutex.hpp"

coro::condition_variable cv;
coro::mutex mtx;
bool ready = false;

async<void> waiter() {
    // 等待会释放互斥锁并暂停协程
    co_await cv.wait(mtx);
    // 等待返回后必须手动重新获取锁
    co_await mtx.lock();

    // 或使用带谓词的版本，会自动重新获取锁
    // co_await cv.wait(mtx, [&]{ return ready; });
}

async<void> notifier() {
    {
        auto guard = co_await mtx.scoped_lock();
        ready = true;
    }
    // 唤醒一个等待的协程
    cv.notify_one();
    // 或唤醒所有等待的协程
    // cv.notify_all();
}
```

等待中的协程必须一直存活到被通知。`wait()` 与解锁路径并发发生的通知会被正确处理，但不带谓词的
`wait()` 返回后仍然不会自动重新获取用户互斥锁。

### semaphore

计数信号量，用于控制对有限数量资源的访问。

```cpp
#include "coro/semaphore.hpp"

async<void> example() {
    // 创建具有 3 个许可的信号量
    coro::counting_semaphore sem(3);

    // 获取许可（如果不可用则暂停）
    co_await sem.acquire();

    // 或获取多个许可
    // co_await sem.acquire(2);

    // 释放许可
    sem.release();

    // 或释放多个许可
    // sem.release(2);

    // 尝试获取而不阻塞
    if (sem.try_acquire()) {
        // 获取成功
        sem.release(); // 记得释放
    }

    // 检查可用许可数
    int available = sem.available();

    // 对于二进制信号量（类似互斥锁的行为）
    // coro::binary_semaphore binary_sem(1);
}
```

`counting_semaphore(initial, max)` 要求 `0 <= initial <= max`。`acquire(n)` 和 `release(n)` 要求 `n > 0`，
并且 `release(n)` 不能让可用许可数超过 `max`。当已有协程排队等待时，许可会优先授予队首等待者。

### channel

Go 风格的 channel 实现，用于协程间通信：

#### 无缓冲 Channel

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

    co_await spawn_local(producer(ch));
    co_await spawn_local(consumer(ch));
}
```

#### 缓冲 Channel

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

#### Channel 广播

该库还支持广播功能，可以同时向所有等待的接收者发送值：

```cpp
#include "coro/channel.hpp"

async<void> broadcast_example() {
    channel<int> ch;  // 用于广播示例的无缓冲 channel

    // 多个接收者等待数据
    auto receiver = [](channel<int>& ch, int id) -> async<void> {
        auto val = co_await ch.recv();
        if (val.has_value()) {
            std::cout << "接收者 " << id << " 收到: " << *val << std::endl;
        }
    };

    // 启动多个接收者
    co_await spawn_local(receiver(ch, 1));
    co_await spawn_local(receiver(ch, 2));
    co_await spawn_local(receiver(ch, 3));

    // 广播将值发送给所有等待的接收者
    size_t notified_count = co_await ch.broadcast(42);
    std::cout << "广播通知了 " << notified_count << " 个接收者" << std::endl;

    // 所有 3 个接收者都将收到值 42
}
```

广播与常规发送的区别：

- `send()` 只向一个接收者发送（如果没有接收者则阻塞）
- `broadcast()` 同时向所有当前等待的接收者发送
- `broadcast()` 返回被通知的接收者数量
- 如果没有接收者在等待，`broadcast()` 会立即完成，不进行缓冲

### wait_group

等待组，类似于 Go 的 `sync.WaitGroup`，用于协调多个协程。

```cpp
#include "coro/wait_group.hpp"

async<void> worker_task(coro::wait_group& wg, std::string name, int work_ms) {
    // 执行一些工作
    co_await sleep(work_ms * 1ms);
    std::cout << name << " completed\n";

    // 信号完成
    wg.done();  // 或 wg.add(-1);
}

async<void> example() {
    coro::wait_group wg;

    // 添加 2 个需要等待的操作
    wg.add(2);

    // 启动工作协程
    spawn(executor, worker_task(wg, "Worker1", 100));
    spawn(executor, worker_task(wg, "Worker2", 150));

    // 等待所有操作完成
    co_await wg.wait();
    // 或直接使用 co_await: co_await wg;

    // 检查当前计数
    int count = wg.get_count();
}
```

计数不能变成负数。还有未完成工作或等待协程时销毁 wait_group 是无效用法。

### latch

倒计时门闩，允许协程等待直到指定数量的操作完成。

```cpp
#include "coro/latch.hpp"

async<void> example() {
    // 创建计数为 3 的门闩
    coro::latch latch(3);

    // 在其他协程中，进行计数递减：
    // latch.count_down(); // 由不同协程调用 3 次

    // 等待门闩计数到达零
    co_await latch.wait();
    // 或直接使用 co_await: co_await latch;

    // 另一种方式：计数递减并等待一次性完成
    // co_await latch.arrive_and_wait();

    // 检查当前计数
    int current_count = latch.get_count();
}
```

初始计数必须非负，`count_down(n)` 不能把计数减到 0 以下。

### event

事件同步原语，允许一个或多个协程等待直到事件被设置。

```cpp
#include "coro/event.hpp"

coro::event evt;

async<void> waiter() {
    // 等待事件被设置
    co_await evt.wait();
    // 或直接使用 co_await: co_await evt;
}

async<void> setter() {
    // 设置事件，唤醒所有等待者
    evt.set();

    // 清除事件（未来的等待将阻塞，直到再次调用 set()）
    // evt.clear();

    // 检查事件是否已设置（非阻塞）
    bool is_set = evt.is_set();
}
```

`event` 是 manual-reset event：`set()` 和 `clear()` 都是幂等的状态切换。`set()` 会释放所有当前等待者，
并让后续等待者直接通过，直到调用 `clear()`。

## 协程本地存储

协程本地存储提供了类似 `thread_local` 的存储机制，但作用域是协程。每个 `coro_local<T>` 实例作为一个唯一的键来存储值，类似于
`thread_local`，但针对协程。

### 基本用法

```cpp
#include "coro/coro_local.hpp"

// 定义存储键（类似 thread_local，通常为 static）
static coro::coro_local<int> request_id;
static coro::coro_local<std::string> user_name;

async<void> process_request() {
    // 为当前协程设置值
    co_await request_id.set(42);
    co_await user_name.set("Alice");

    // 获取值（如果未设置则返回默认构造的 T）
    int id = co_await request_id.get();
    std::string name = co_await user_name.get();

    std::cout << "处理请求 " << id << "，用户 " << name << std::endl;
}
```

### API 参考

```cpp
coro::coro_local<T> storage;

// 为当前协程设置值
co_await storage.set(value);

// 获取值（如果未设置则返回默认值 T{}）
T value = co_await storage.get();

// 获取 optional（如果未设置则返回 std::nullopt）
std::optional<T> opt = co_await storage.get_optional();

// 获取指针（如果未设置则返回 nullptr）
T* ptr = co_await storage.get_ptr();

// 检查值是否存在
bool exists = co_await storage.has();

// 删除值
co_await storage.erase();
```

### 从父协程继承

子协程可以继承父协程的值。默认情况下，子协程可以读取父协程的值。使用 `inherit_coro_local()` 启用写时复制语义，使修改仅对子协程生效。

```cpp
static coro::coro_local<int> context_id;

async<void> parent_coro() {
    co_await context_id.set(100);

    // 子协程可以读取父协程的值
    auto child = []() -> async<void> {
        int id = co_await context_id.get();
        std::cout << "子协程看到: " << id << std::endl;  // 输出: 100
        co_return;
    };

    co_await child();
}
```

### 写时复制语义

当你希望子协程继承值但拥有自己的副本进行修改时，使用 `inherit_coro_local()`：

```cpp
async<void> parent_coro() {
    co_await context_id.set(100);

    auto child = []() -> async<void> {
        // 启用写时复制继承
        co_await coro::inherit_coro_local();

        // 读取父协程的值
        int id = co_await context_id.get();  // 100

        // 本地修改（不影响父协程）
        co_await context_id.set(200);

        id = co_await context_id.get();  // 200
        co_return;
    };

    co_await child();

    // 父协程的值保持不变
    int id = co_await context_id.get();  // 仍然是 100
}
```

### 并发协程之间的隔离

每个协程都有自己独立的存储空间。并发协程之间互不干扰：

```cpp
async<void> concurrent_example() {
    static coro::coro_local<int> task_id;

    auto task_a = []() -> async<void> {
        co_await task_id.set(111);
        co_await coro::sleep(50ms);
        int id = co_await task_id.get();  // 仍然是 111
        co_return;
    };

    auto task_b = []() -> async<void> {
        co_await task_id.set(222);
        co_await coro::sleep(50ms);
        int id = co_await task_id.get();  // 仍然是 222
        co_return;
    };

    // 两个任务并发运行，存储空间相互隔离
    co_await coro::spawn_local(task_a());
    co_await coro::spawn_local(task_b());
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

## ASIO 适配器

该库提供了 `coro::async<T>` 和 `asio::awaitable<T>` 之间的互操作适配器，让你可以无缝混合使用 coro 和 ASIO 协程。

### asio_executor

`asio_executor` 将 `asio::io_context` 适配为 `coro::executor` 接口，使 coro 协程可以在 ASIO 事件循环上运行：

```cpp
#include "coro/adaptor/asio_adaptor.hpp"

asio::io_context io_ctx;
coro::asio_executor exec(io_ctx);

// 在 ASIO 事件循环上启动 coro 协程
spawn(exec, my_coro_task());

// 运行 ASIO 事件循环
io_ctx.run();
```

### await_asio

`await_asio()` 允许 coro 协程 `co_await` ASIO 的可等待对象：

```cpp
#include "coro/adaptor/asio_adaptor.hpp"

// ASIO 协程
asio::awaitable<int> asio_fetch_data() {
    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    timer.expires_after(100ms);
    co_await timer.async_wait(asio::use_awaitable);
    co_return 42;
}

// coro 协程调用 ASIO 协程
coro::async<void> coro_task() {
    // 使用 await_asio 在 coro 中调用 ASIO 可等待对象
    int result = co_await coro::await_asio(asio_fetch_data());
    std::cout << "Got: " << result << std::endl;
    
    // 也支持 void 返回类型
    co_await coro::await_asio(some_asio_void_task());
}
```

### await_coro

`await_coro()` 允许 ASIO 协程 `co_await` coro 的异步任务：

```cpp
#include "coro/adaptor/asio_adaptor.hpp"

// coro 协程
coro::async<int> coro_compute() {
    co_await coro::sleep(100ms);
    co_return 99;
}

// ASIO 协程调用 coro 协程
asio::awaitable<void> asio_task() {
    // 使用 await_coro 在 ASIO 中调用 coro 异步任务
    int result = co_await coro::await_coro(coro_compute());
    std::cout << "Got: " << result << std::endl;
    
    // 也支持 void 返回类型
    co_await coro::await_coro(some_coro_void_task());
}
```

### 嵌套调用

你可以进行深层嵌套的跨框架调用：

```cpp
// coro -> asio -> coro
coro::async<int> coro_task() {
    // 调用 ASIO 协程，该协程内部又调用 coro 协程
    int result = co_await coro::await_asio(asio_intermediate());
    co_return result;
}

asio::awaitable<int> asio_intermediate() {
    // 在 ASIO 中调用 coro 协程
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

### 异常处理

异常可以正确地在框架边界之间传播：

```cpp
// 在 coro 中捕获 ASIO 抛出的异常
coro::async<void> coro_catches_asio_exception() {
    try {
        co_await coro::await_asio(asio_throws());
    } catch (const std::exception& e) {
        std::cout << "捕获: " << e.what() << std::endl;
    }
}

// 在 ASIO 中捕获 coro 抛出的异常
asio::awaitable<void> asio_catches_coro_exception() {
    try {
        co_await coro::await_coro(coro_throws());
    } catch (const std::exception& e) {
        std::cout << "捕获: " << e.what() << std::endl;
    }
}
```

### 完整示例

```cpp
#include "coro/coro.hpp"
#include "coro/adaptor/asio_adaptor.hpp"

using namespace coro;

async<void> main_task() {
    // 自由混合 coro 和 ASIO 协程
    co_await sleep(50ms);  // coro 定时器
    co_await await_asio(asio_sleep(50ms));  // ASIO 定时器
    
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
./coro_condition_variable
./coro_event
./coro_latch
./coro_semaphore
./coro_wait_group
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
│       ├── condition_variable.hpp # 条件变量
│       ├── event.hpp         # 事件同步原语
│       ├── latch.hpp         # 门闩
│       ├── mutex.hpp         # Mutex
│       ├── semaphore.hpp     # 信号量
│       ├── wait_group.hpp    # 等待组
│       └── when.hpp          # when_all/when_any
└── test/                     # 测试文件
```
