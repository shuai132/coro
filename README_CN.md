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
| `coro::counting_semaphore`                   | 计数信号量，用于资源控制                       |
| `coro::wait_group`                           | 等待组，用于协调多个协程                       |
| `coro::channel<T>`                           | Go 风格的 channel，用于协程间通信             |
| `coro::executor`                             | 执行器基类接口                            |
| `coro::executor_loop`                        | 基于事件循环的执行器                         |
| `coro::executor_poll`                        | 基于轮询的执行器                           |
| `coro::current_executor()`                   | 获取当前执行器                            |
| `coro::callback_awaiter<T>`                  | 将回调式 API 转换为协程                     |
| `coro::coro_local<T>`                        | 协程本地存储（类似 thread_local）            |
| `coro::inherit_coro_local()`                 | 确保当前协程的本地存储存在                   |

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

- 正确支持 C++20 协程的编译器和标准库（需要 `<coroutine>` 与 `std::coroutine_handle`）
- CMake 3.15+（可选，用于构建测试）

GCC 构建需要启用尾调用优化，才能避免连续等待立即完成的子协程时不断增长调用栈。
CMake 的 `coro` 目标会为 GCC 调用方启用 `-foptimize-sibling-calls`（包括 Debug 构建）；直接使用头文件时请手动添加。
该选项也会对调用方目标中的其他 C++ 函数启用尾调用优化，可能改变调试器中的调用栈。请在实际目标 ABI 和工具链上验证栈使用情况。

GCC 10 还需要 `-fcoroutines`；CMake 的 `coro` 目标会自动添加此选项。

GCC 10.3 无法通过协程回归测试：部分左值 awaiter 无法编译，临时聚合 awaiter 还可能提前释放共享所有权。请使用能够通过测试套件的工具链，仅启用 C++20 模式不足以证明兼容性。

在没有 `std::mutex` 的裸机标准库上，按需包含组件头文件（例如 `coro/channel.hpp`），使用 `_st` 别名或显式传入互斥锁模板参数。
这些工具链不提供依赖原生互斥锁的默认类型和 `_mt` 别名。需要提供平台执行器，并在禁用异常编译时定义 `CORO_DISABLE_EXCEPTION`。
总入口 `coro.hpp`、内置执行器、`when_*` 和协程泄漏追踪目前仍要求标准线程支持。

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

`async<T>` 的 `co_return` 同时支持表达式和列表初始化，包括只能移动的聚合返回值。

```cpp
#include <iostream>
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

    // 启动任务，并在任务完成时停止事件循环
    process().detach_with_callback(executor, [&executor] {
        executor.stop();
    });

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

`bind_executor(exec)` 指定任务的执行器。直接 `co_await` 绑定到其他执行器的子任务时，子任务在指定执行器上运行，等待方在自己的执行器上继续；未绑定的子任务继承调用方的执行器。

## 执行器 (Executor)

目前提供两种执行器实现：

内建执行器和 `asio_executor` 会将 `post_delayed_ns()` 无法表示的截止时间饱和到单调时钟的最大时间点。

内建执行器公平调度已到期的定时任务和普通排队任务，避免任一队列持续追加任务时使另一队列饿死。

### executor_loop

基于条件变量的事件循环，适合作为主线程运行：

```cpp
#include "coro/executor_loop.hpp"

executor_loop executor;

// 启动协程...

// 阻塞运行直到 stop() 被调用
executor.run_loop();
```

调用 `stop()` 后，`run_loop()` 会执行完已入队的任务（包括定时任务）再返回。

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

`sleep(duration)` 和直接 `co_await duration` 会将非正时长作为零延迟任务提交给执行器。
`sleep(duration)` 和 `delay(ms)` 会将超出纳秒表示范围的正时长饱和到 `std::chrono::nanoseconds::max()`。

## 并发操作

### when_all

等待所有任务完成：

启用异常时，`when_all()` 会等待所有已启动的子任务结束，再抛出最先记录的错误。
如果某个子任务启动失败，后续子任务不会启动。

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

`when_any()` 至少需要一个 awaitable。它会在第一个任务完成时返回，但不会取消其他任务；较慢的任务会继续在各自
executor 上运行。

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
    auto guard = co_await mtx.scoped_lock();
    while (!ready) {
        // 等待会释放已持有的互斥锁并暂停协程
        co_await cv.wait(mtx);
        // 重新获取锁后再检查条件
        co_await mtx.lock();
    }

    // 或在持有锁的前提下，用带谓词的版本替换上面的循环：
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
#include "coro/when.hpp"

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

    // 等待两个任务结束，再销毁局部 channel。
    co_await when_all(producer(ch), consumer(ch));
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
#include <iostream>
#include "coro/channel.hpp"
#include "coro/executor_poll.hpp"

using namespace coro;

async<void> receive_once(channel<int>& ch, int id) {
    if (auto value = co_await ch.recv()) {
        std::cout << "接收者 " << id << " 收到: " << *value << std::endl;
    }
}

async<void> broadcast_once(channel<int>& ch) {
    size_t notified = co_await ch.broadcast(42);
    std::cout << "广播通知了 " << notified << " 个接收者" << std::endl;
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
        exec.poll();  // 确保此接收者已进入 recv()，再进行广播。
    }

    broadcast_once(ch).detach_with_callback(exec, on_finished);
    while (finished != 4) {
        exec.poll();  // 等待全部四个任务结束，再销毁 channel。
    }
    return 0;
}
```

广播与常规发送的区别：

- `send()` 只向一个接收者发送（如果没有接收者则阻塞）
- `broadcast()` 同时向所有当前等待的接收者发送
- `broadcast()` 返回被通知的接收者数量
- 如果没有接收者在等待，`broadcast()` 会立即完成，不进行缓冲

复制广播值抛出异常时，所有接收者仍保持等待，且不会收到部分广播；调用方可以重试或关闭通道。

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
    co_return;
}
```

`event` 是 manual-reset event：`set()` 和 `clear()` 都是幂等的状态切换。`set()` 会释放所有当前等待者，
并让后续等待者直接通过，直到调用 `clear()`。
对于绑定了执行器的已挂起等待者，`set()` 将后续执行加入对应执行器的队列。没有执行器的等待者则直接在调用线程恢复。

## 协程本地存储

协程本地存储提供了类似 `thread_local` 的存储机制，但作用域是协程。每个 `coro_local<T>` 实例作为一个唯一的键来存储值，类似于
`thread_local`，但针对协程。

### 基本用法

在包含任何 coro 头文件前启用 `CORO_ENABLE_LOCAL_STORAGE`，并确保所有编译单元的配置一致。
使用 CMake 时，配置 `-DCORO_ENABLE_LOCAL_STORAGE=ON` 并链接 `coro` 目标即可传递此定义。

```cpp
#ifndef CORO_ENABLE_LOCAL_STORAGE
#define CORO_ENABLE_LOCAL_STORAGE
#endif
#include <iostream>
#include <string>
#include "coro/coro_local.hpp"

// 定义存储键（类似 thread_local，通常为 static）
static coro::coro_local<int> request_id;
static coro::coro_local<std::string> user_name;

coro::async<void> process_request() {
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

// 获取借用指针（可能指向祖先协程中的值；未设置时返回 nullptr）
T* ptr = co_await storage.get_ptr();

// 检查值是否存在
bool exists = co_await storage.has();

// 删除本地覆盖值（继承的值会重新可见）
co_await storage.erase();
```

### 从父协程继承

通过 `co_await` 直接等待的子协程会自动继承对父协程值的访问。调用 `set()` 会创建或替换本地值，不改变父协程的值。分离任务从独立的存储开始。
`inherit_coro_local()` 仅确保当前协程的存储映射存在；继承和本地写入都不要求调用它。

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

### 本地覆盖

使用 `set()` 在子协程中覆盖继承的值。`erase()` 仅删除本地覆盖值，使继承的值重新可见。
`get_ptr()` 返回指向存储值的借用指针，该值可能属于祖先协程；通过此指针修改会改变祖先协程的值。
如果希望修改仅在本地生效，使用 `get()` 读取，再用 `set()` 写回：

```cpp
async<void> parent_coro() {
    co_await context_id.set(100);

    auto child = []() -> async<void> {
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

等待它的协程必须已绑定或继承执行器。两个重载的区别是注册函数是否接收该执行器参数。

启用异常时，存储或取出回调结果期间发生的异常会在 `co_await` 处重新抛出。

```cpp
// 注册函数不接收执行器参数
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

// 注册函数接收执行器参数
async<void> async_void_operation() {
    co_await callback_awaiter<void>([](executor* exec, auto callback) {
        // 可以使用执行器进行调度
        exec->post_delayed_ns(std::move(callback), 1000000);  // 1ms 后执行
    });
}
```

## ASIO 适配器

该库提供了 `coro::async<T>` 和 `asio::awaitable<T>` 之间的互操作适配器，让你可以无缝混合使用 coro 和 ASIO 协程。

两个适配器均支持只能移动的返回值，无需默认构造或赋值能力。

从 ASIO strand 中等待的未绑定 coro 任务会继承该 strand，嵌套 `await_asio()` 调用也保留这一调度约束。

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

`await_asio()` 允许 coro 协程 `co_await` ASIO 的可等待对象。等待中的 coro 任务必须绑定或继承 `asio_executor`：

停止 `io_context` 会暂停已入队的后续执行；调用 `restart()` 并再次 `run()` 后，等待中的 coro 任务可以继续。
执行器和任务引用的数据都必须存活到任务结束。

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

`await_coro()` 允许 ASIO 协程 `co_await` coro 的异步任务。ASIO 协程必须运行在 `asio::io_context` 的执行器或基于该上下文的 strand 上；
不支持 `asio::thread_pool` 等其他上下文：

取消 ASIO 等待时，如果 coro 任务尚未完成，会报告 `asio::error::operation_aborted`。
已分离的 coro 任务会继续执行，因此它引用的数据必须存活到任务结束。

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

配置宏和调试宏必须在包含任何 coro 头文件前定义，并在所有使用该库的编译单元中保持一致。建议通过公共配置头文件或作用于整个构建目标的宏定义统一设置。混用 `CORO_DISABLE_EXCEPTION` 等配置可能改变 promise 的内存布局，违反单一定义规则，即使 Debug 构建看似正常也不安全。

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

日志钩子在调用线程上执行，可能被并发调用，请使用线程安全的日志函数。泄漏数量是记录对应事件时的快照。

调试追踪器将共享记录表保留至进程退出，使静态对象的构造和析构阶段也能安全创建协程。

泄漏追踪会保留编译器请求的分配对齐。Clang 编译过度对齐的协程状态时，可能需要启用 `-fcoro-aligned-allocation`。

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

### GDB / LLDB 异步调用链

构建应用时启用可选诊断信息（默认关闭）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCORO_DEBUG_ASYNC_STACK=ON
cmake --build build -j
```

不使用 CMake 时，在包含任何 coro 头文件之前定义 `CORO_DEBUG_ASYNC_STACK`，并启用调试信息
（`-g`，MSVC 使用 `/Zi`）。**所有翻译单元必须保持宏配置一致**，因为该选项会改变 promise 布局。
CMake 会通过 `coro` 接口目标传播宏。此功能不依赖 `CORO_DEBUG_PROMISE_LEAK`。

调试器需要启用 Python 3 脚本支持。加载脚本、暂停进程，然后列出协程，按 ID 或协程帧地址查看调用链：

```text
(gdb) source tools/debugger/coro_debug.py
(gdb) coro-list
(gdb) coro-bt id:4
(gdb) coro-bt 0x12345678

(lldb) command script import tools/debugger/coro_debug.py
(lldb) coro-list
(lldb) coro-bt id:4
```

输出示例（地址和 ID 会变化）：

```text
#0 id:4 frame=0x12345678 [live] async<int> debug_leaf() at test/coro_debug.cpp:28
#1 id:3 frame=0x12345000 [live] async<int> debug_middle() at test/coro_debug.cpp:33
#2 id:2 frame=0x12344000 [live] async<void> debug_root(int &) at test/coro_debug.cpp:37
```

脚本跟踪 `co_await` 父子关系，包括 `when_all` / `when_any` 的内部包装协程，支持跨线程恢复。
每个 promise 内含侵入式诊断记录和生命周期内唯一的 ID，协程帧销毁时移除记录，防止地址复用导致
错误关联。`when_any` 的落后分支可能比父协程活得更久，此时脚本会明确报告父协程已不存在。
普通 `spawn` / `detach` 启动独立调用链，不会自动关联启动它的代码；遇到外部 promise 时，
脚本会提示缺少诊断信息并终止追踪。

这是对原生 `bt` 的补充，展示的是**逻辑异步调用链**。源码位置标识协程函数，并非当前指令或每个
挂起点。`[live]` 表示尚未到达最终挂起点，不区分正在运行、挂起和从未启动；已完成但尚未销毁的
帧显示为 `[completed]`，其父链接已经清除。
标准库缺少 `std::source_location` 或使用 GCC 14 之前的版本时，改用编译器内建函数采集源码位置；
该配置下的函数名不包含完整类型签名。

两个命令只读取内存，不调用被调试进程中的函数、不获取其锁，也不依赖编译器的协程帧 ABI。
使用前需要**暂停全部线程**。对于包含注册表、协程诊断记录和引用字符串的 core dump，只要提供
匹配的调试符号，也可使用这些命令。精简 core 缺少只读映像页时，LLDB 脚本会尝试从匹配二进制
读取源码字符串。普通 Breakpad minidump 可能缺少必要的协程帧内存，本功能并非
Breakpad 栈展开器扩展。注册表更新中断、链表损坏、内存缺失和遍历超限时会明确报错或停止，
遍历上限为 4096 条记录和 256 层调用链。

启用后，每个 promise 增加诊断字段，注册、关联父协程、完成和销毁时需要获取一个全局互斥锁；
每条记录不额外分配堆内存。关闭时不编译这些字段和同步操作。优化构建仍需保留匹配的调试信息，
用于线上 dump 前应验证实际使用的编译器和调试器组合。

运行回归用例和调试器验证：

```bash
./build/coro_debug
python3 test/debugger_test.py
python3 test/debugger_smoke.py gdb build/coro_debug
python3 test/debugger_smoke.py lldb build/coro_debug --core
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

启用 ASIO 适配器测试时，在配置阶段通过环境变量 `ASIO_PATH` 指定包含 `asio.hpp` 的头文件目录。
构建目录会保留该路径，供后续重新配置使用。需要更换路径时重新设置该环境变量；显式设为空值可禁用
ASIO 测试。禁用异常时仍使用占位测试。

### CMake 选项

| 选项                             | 默认值         | 说明                  |
|--------------------------------|-------------|---------------------|
| `CORO_BUILD_TEST`              | ON (当作为主项目) | 构建测试                |
| `CORO_TEST_RUNNER_VERY_SLOW`   | OFF         | 为慢速测试环境放宽时间限制      |
| `CORO_ENABLE_SANITIZE_ADDRESS` | OFF         | 启用 AddressSanitizer |
| `CORO_ENABLE_SANITIZE_THREAD`  | OFF         | 启用 ThreadSanitizer  |
| `CORO_DISABLE_EXCEPTION`       | OFF         | 禁用异常支持              |
| `CORO_DEBUG_ASYNC_STACK`       | OFF         | 启用异步调用链诊断信息 |

macOS GitHub Actions runner 会自动启用 `CORO_TEST_RUNNER_VERY_SLOW`。

## 项目结构

```
coro/
├── include/
│   ├── coro.hpp              # 主头文件（包含所有组件）
│   └── coro/
│       ├── coro.hpp          # 核心协程实现
│       ├── executor.hpp      # 执行器接口
│       ├── executor_base.hpp  # 基础任务执行器
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
