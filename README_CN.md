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
- [最小可运行示例](#最小可运行示例)
- [执行器 (Executor)](#执行器-executor)
- [线程安全契约](#线程安全契约)
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
- [协作式停止与收尾](#协作式停止与收尾)
- [协程本地存储](#协程本地存储)
- [回调转协程](#回调转协程)
- [ASIO 适配器](#asio-适配器)
- [MCU 场景：coro 与 RTOS](#mcu-场景coro-与-rtos)
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

标准库提供 `std::mutex` 且允许链接探测时，CMake 目标会传递检测到的原生线程依赖。
无线程组件构建不引入原生线程库。直接使用头文件或仅允许编译探测的交叉工具链，需自行提供平台
要求的线程参数，例如旧版 Linux 运行库所需的 `-pthread`。

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

已完成的任务可以再次等待，协程体不会重新执行。每次成功等待都会移出非 `void` 结果，因此后续等待看到的是移动后的值；
需要重复使用结果时，请保留首次取出的值。已保存的异常会在每次等待时重新抛出。同一任务只能有一个活动的等待者，
等待期间任务对象必须存活且不能移动。销毁未启动的任务会释放其帧；销毁已启动但尚未完成的任务会将其分离，
不会取消操作，其引用的数据和执行器必须存活到任务完成。通过协程句柄手动恢复任务也遵循同样的规则。
通过 `detach()` 或 `spawn()` 启动已完成的任务时，只会释放其协程帧，不会重新执行协程体。

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

`bind_executor(exec)` 只能在任务启动前调用。直接 `co_await` 绑定到其他执行器的子任务时，子任务在指定执行器上运行，等待方在自己的执行器上继续；未绑定的子任务继承调用方的执行器。
这种跨执行器的直接等待通过队列启动子任务并恢复等待方，避免共用线程的执行器之间发生递归调度。

## 最小可运行示例

每个文件只展示一个组件的基本用法，包含完整的 `main()`，不依赖测试工具。建议从 [task.cpp](examples/task.cpp) 开始。

| 文件 | 用法 |
|------|------|
| [task.cpp](examples/task.cpp) | 创建任务、等待结果、执行器继承 |
| [mutex.cpp](examples/mutex.cpp) | RAII 加锁与竞争等待 |
| [channel.cpp](examples/channel.cpp) | 发送、接收、关闭 |
| [broadcast.cpp](examples/broadcast.cpp) | 广播给等待者，使用轮询执行器 |
| [semaphore.cpp](examples/semaphore.cpp) | 限制同时运行的任务数 |
| [condition_variable.cpp](examples/condition_variable.cpp) | 持锁等待谓词与通知 |
| [wait_group.cpp](examples/wait_group.cpp) | 登记任务并等待全部完成 |
| [latch.cpp](examples/latch.cpp) | 一次性集合点 |
| [event.cpp](examples/event.cpp) | 等待与发出信号 |
| [when.cpp](examples/when.cpp) | 等待所有任务或最先完成者 |
| [timer.cpp](examples/timer.cpp) | 异步延时 |
| [callback.cpp](examples/callback.cpp) | 将异步回调转成 co_await |
| [coro_local.cpp](examples/coro_local.cpp) | 设置并继承协程本地值 |

```bash
cmake -S . -B build -DCORO_BUILD_TEST=OFF -DCORO_BUILD_EXAMPLES=ON
cmake --build build --config Debug -j
ctest --test-dir build -C Debug -L examples --output-on-failure

# 运行单个示例（Windows/MSVC：build/Debug/coro_example_task.exe）
./build/coro_example_task
```

示例与测试保持一致：作为主项目时默认开启，作为子项目引入时默认关闭，可通过 `-DCORO_BUILD_EXAMPLES=ON/OFF` 覆盖。无异常模式额外传入 `-DCORO_DISABLE_EXCEPTION=ON`。示例使用单个执行器线程；同步原语也可以换成对应的 `_st` 类型。`coro_local.cpp` 自行开启本地存储。CI 会在现有桌面平台的 Debug/Release、异常开关及 sanitizer 矩阵中编译并运行全部示例。

## 执行器 (Executor)

目前提供两种执行器实现：

内建执行器和 `asio_executor` 会将 `post_delayed_ns()` 无法表示的截止时间饱和到单调时钟的最大时间点。

内建执行器公平调度已到期的定时任务和普通排队任务，避免任一队列持续追加任务时使另一队列饿死。
用户回调对象的移动和销毁都在队列锁之外进行，因此捕获的资源可以在清理时再次投递任务。
普通入队回调有一次用于所有权管理的分配。启用 RTTI 时，内建协程续体只保存句柄；直接执行的 `dispatch()` 也不需要这次分配。

### executor_loop

基于条件变量的事件循环，适合作为主线程运行：

```cpp
#include "coro/executor_loop.hpp"

executor_loop executor;

// 启动协程...

// 阻塞运行直到 stop() 被调用
executor.run_loop();
```

调用 `stop()` 后，会唤醒所有空闲的 `run_loop()` 线程。循环继续处理已入队的任务（包括定时任务），直到队列清空。

只有调用线程处于该执行器的 `run_loop()` 范围内（包括嵌套调用）时，`dispatch()` 才会直接执行。
循环正常返回或因异常退出后，该线程不再被视为活动的循环执行上下文。

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
    // 可选：重写 post_continuation(fn)，在 stop() 后排空已接收任务时保留恢复通知。
    void post(std::function<void()> fn) override;          // 稍后执行
    void post_delayed_ns(std::function<void()> fn, uint64_t delay_ns) override;  // 延迟执行
};
```

`coro::executor` 只定义调度操作。`stop()` 属于 `executor_loop`、`executor_poll` 等具体事件循环类型；
外部事件循环由调用方通过所属上下文停止。

## 线程安全契约

推荐单线程运行，每个执行器固定一个线程，外部线程通过投递任务参与，兼顾性能和较低的心智负担。
`executor_loop` 也支持多线程运行，但恢复线程不固定，共享数据需要同步；`executor_poll` 必须由固定线程轮询。
跨线程共享同步原语使用 `_mt`，`_st` 的操作和通知均应在同一线程执行。同一个任务或 awaiter 不可并发操作。

子协程默认继承父执行器，也可显式绑定；已绑定的协程通过自己的执行器恢复，立即就绪的操作原地继续。
`stop()` 不取消协程：`executor_loop` 继续处理已排队任务和定时器，`executor_poll` 不再取任务。
应先等待协程和外部回调完成，再停止执行器、join 线程并销毁相关对象。

### 默认名称、`_mt` 与 `_st` 的选择

对于提供这组名称的同步原语，默认名称与 `_mt` 使用相同的多线程安全实现，`_st` 则要求单线程使用。以 `wait_group` 为例：

| 名称 | 实际类型 | 适用场景 |
|------|----------|----------|
| `wait_group` | `wait_group_t<std::mutex>` | 通用默认选择，使用内部互斥锁保护状态 |
| `wait_group_mt` | `wait_group_t<std::mutex>` | 与默认名称完全相同，显式表达跨线程使用意图 |
| `wait_group_st` | `wait_group_t<dummy_mutex>` | 不使用内部互斥锁，由调用者保证所有操作和通知都在同一线程执行 |

`event`、`latch`、`condition_variable`、`counting_semaphore` 和 `binary_semaphore` 也遵循这一约定。
标准库支持 `std::mutex` 时，`channel<T>` 的默认互斥锁参数同样是 `std::mutex`，与 `channel_mt<T>` 相同；
`channel_st<T>` 使用 `dummy_mutex`。`coro::mutex` 是基于原子操作的独立实现，不提供 `mutex_mt` / `mutex_st` 别名。

选择建议：

- 通用代码或不能确定调用线程时，优先使用默认名称；跨线程共享时可用 `_mt` 明确表达意图。
- 能保证全部操作都在同一线程时，可显式使用 `_st`，避免内部互斥锁开销，例如由一个固定线程运行的事件循环。
  默认版本在单线程中也能使用；`_st` 不代表完全没有原子操作、分配或其他开销。
- 判断依据是所有操作的调用线程，包括 `done()`、`set()`、查询和通知，而不只是等待协程在哪个线程运行。
  即使只有一个等待线程，另一个线程直接调用 `done()` 也需要默认版本或 `_mt`。
  外部线程也可以通过线程安全的投递接口，将操作交给 `_st` 所属线程执行。
- 多线程安全只保护原语自身的内部状态，不自动保护业务数据，也不允许并发操作同一个 task / awaiter，
  或在仍有使用者时销毁对象。
- 标准库不提供 `std::mutex` 时，这些默认别名、`_mt` 别名及对应的默认模板参数不可用；
  使用组件头文件配合 `_st`，或显式指定适合平台的互斥锁类型。库不会自动把默认名称切换为单线程版本。

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

绑定到执行器时，非空的 `when_all()` 和 `when_any()` 会将子任务的首次启动和已挂起调用方的恢复放入执行器队列，
避免深层嵌套的组合器不断增加原生调用栈。

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
如需停止剩余任务并安全释放共享资源，参见[协作式停止与收尾](docs/cooperative-stop_CN.md)。

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
`wait()` 返回后仍然不会自动重新获取用户互斥锁。只有内部队列加锁和用户互斥锁解锁都已返回，
通知才可能恢复等待者。

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

只有获取方完成等待节点注册并释放内部队列锁，`release()` 才可能恢复该协程。

`counting_semaphore(initial, max)` 要求 `0 <= initial <= max`。`acquire(n)` 和 `release(n)` 要求 `n > 0`，
并且 `release(n)` 不能让可用许可数超过 `max`。当已有协程排队等待时，许可会优先授予队首等待者。

### channel

Go 风格的 channel 实现，用于协程间通信：

绑定到执行器的等待发送者和接收者通过执行器队列恢复，关闭通道时同样如此，避免通道流水线不断增加原生调用栈。
未绑定执行器的等待者会直接恢复。

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

## 协作式停止与收尾

使用 `event` 广播停止请求，由任务协作收尾，再用 `wait_group` 等待全部退出。
`when_any` 返回后，其余分支仍会运行，需要显式通知并等待收尾。

完整示例、异常处理和生命周期说明见[协作式停止与收尾](docs/cooperative-stop_CN.md)。

## 协程本地存储

协程本地存储提供了类似 `thread_local` 的存储机制，但作用域是协程。每个 `coro_local<T>` 实例作为一个唯一的键来存储值，类似于
`thread_local`，但针对协程。

### 基本用法

在包含任何 coro 头文件前启用 `CORO_ENABLE_LOCAL_STORAGE`，并确保所有编译单元的配置一致。
使用 CMake 时，配置 `-DCORO_ENABLE_LOCAL_STORAGE=ON` 并链接 `coro` 目标即可传递此定义。

存储键按对象地址识别。当前协程或其子协程仍可能访问相关存储项时，键应保持存活且地址稳定。
销毁键不会删除已存储的值；若在同一地址构造新键，新键可能读到旧存储项。
使用短生命周期的键时，应在复用地址前删除相关存储项，或等所有可能访问这些存储项的协程结束。
`erase()` 只删除当前协程的本地项，继承项可能仍然存在。全局或 static 键通常更容易满足这一生命周期要求。

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
即使先释放父任务，继承的存储也会保留到子协程的本地值析构完成。
释放保留的子任务时，独占的祖先存储会逐层迭代释放，避免析构递归随继承深度增长。

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

通过 `asio_executor` 复用 Asio 执行器，通过 `await_asio()` / `await_coro()` 在两套协程间调用。
Asio 负责 I/O 与底层调度，coro 提供任务和同步组件，应用管理上下文与资源生命周期。
适配层保持可选，按实际需求扩展，不重复封装 I/O 或建立通用兼容框架。

双向桥接支持 move-only 结果，无需默认构造或赋值能力。

### asio_executor

`asio_executor` 接受 `io_context&` 或可转换为 `asio::any_io_executor` 的执行器，包括
`socket.get_executor()`、strand 和 `thread_pool` 执行器。`get_asio_executor()` 返回保存的执行器。

```cpp
#include "coro/adaptor/asio_adaptor.hpp"

asio::io_context io_ctx;
asio::ip::tcp::socket socket(asio::make_strand(io_ctx));
coro::asio_executor exec(socket.get_executor());

spawn(exec, my_coro_task());
io_ctx.run();
```

通过 `spawn(exec, task)` 或在任务启动前调用 `task.bind_executor(exec)` 选择执行器，显式绑定优先，未绑定的子任务继承父任务的执行器。
挂起恢复和嵌套桥接保留 strand 约束；strand 保证串行执行但不固定线程，任务可在挂起点交错执行。

- 适配器及底层上下文必须存活到所有任务和回调结束；适配器地址保持不变，期间不能从它移出或对它重新赋值。
  销毁适配器不会取消任务。
- 适配器不提供 `stop()`。调用方的 `io_ctx.stop()` 停止整个上下文，不取消 I/O，也不排空任务。
  优雅退出时，先取消 I/O 并等待任务和回调结束，再销毁相关资源。
- 如果任务等待外部回调且没有尚未完成的 Asio 工作，按需使用 `asio::make_work_guard(io_ctx)` 保持 `run()` 运行。

### await_asio

`await_asio()` 允许 coro 协程 `co_await` ASIO 的可等待对象。等待中的 coro 任务必须绑定或继承 `asio_executor`：

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

`await_coro()` 允许 ASIO 协程 `co_await` coro 的异步任务。未绑定的 coro 任务继承 ASIO 协程的执行器及其 strand 约束，
也支持 `asio::thread_pool`：

取消生效时，等待报告 `asio::error::operation_aborted`；已确定的完成结果不受后续取消影响。
取消等待不会终止 coro 任务，它引用的数据必须存活到任务结束。

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

## MCU 场景：coro 与 RTOS

在以外设交互和 I/O 等待为主的 MCU 应用中，**coro 配合中断通常是一个轻量、直接的优先选择**：
中断负责及时响应硬件事件，coro 在主循环中组织多条业务流程，既获得并发处理的便利，
又减少了引入 RTOS 后的任务管理工作，以及为每条流程独立预留任务栈的内存开销。

准确来说，coro 特别适合**等待多、流程复杂，但不需要每条流程都有独立抢占优先级**的业务，
例如传感器采集、串口通信、设备初始化和通信重试。
FreeRTOS 等 RTOS 则更适合需要任务优先级、抢占调度，或依赖现有 RTOS 生态的场景。
两者各有所长：已有 RTOS 的工程也可以用 coro 组织任务内部的异步流程，让复杂业务更清晰。

### coro 的优势

- **流程清晰**：用 `co_await` 按顺序表达异步操作，保留局部变量和 RAII，减少回调和手写状态机。
- **内存利用灵活**：多个协程共用执行上下文，无需各自预留独立任务栈；实际占用仍需计入协程帧、队列和执行栈，见[内存部署说明](#协程帧内存与-mcu-部署)。
- **易于组合和接入**：用 `async<T>`、`when_all` 等组合业务，通过执行器和回调适配接入现有驱动；仅头文件，也支持关闭异常。

### 选择与配合

- **裸机、I/O 等待为主**：优先考虑 coro + 中断，由主循环驱动执行器。
- **需要任务级抢占和优先级**：使用 RTOS；coro 本身不提供抢占调度。
- **实时控制与复杂业务并存**：高优先级 RTOS 任务负责控制，另一个任务承载 coro 执行器，组织通信、采集等并发流程。

中断中只做必要的数据记录和通知，协程在主循环或 RTOS 任务上下文中恢复；`_mt` 不代表 ISR 安全。
同一串行执行器上的长时间计算或阻塞调用会延迟其他协程，应控制连续执行的耗时。

本库目前没有内置 FreeRTOS 适配器，需按平台实现 `coro::executor`，对接事件队列、时钟和唤醒。
内置执行器依赖标准库线程、互斥等设施，裸机部署应确认工具链支持，或使用平台执行器。

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

定义 `CORO_DISABLE_EXCEPTION` 后，即使编译器仍支持异常，也会移除库内所有显式 `try`/`catch`、异常保存、
重新抛出以及依赖异常的清理和回滚代码。协程标准要求的 `unhandled_exception()` 钩子保留为
`std::terminate()` 失败终止路径，不捕获、保存或传播错误。

该模式下，库使用的日志钩子、用户值操作、互斥锁操作和执行器回调不得抛异常。库不会捕获诊断日志错误，
也不再提供创建失败、广播复制失败或等待者解锁失败的异常恢复。异常逃出协程体、该模式下的 promise 构造、
初始挂起或其他 `noexcept` 操作时，程序会终止。

该宏不会改变编译器选项，也不能控制标准库或 ASIO 内部的异常处理。编译器仍启用异常时，协程转换仍可能
生成隐式异常处理代码。对于要求完全禁用异常的环境，还需关闭编译器异常，例如 GCC/Clang：

```bash
c++ -std=c++20 -fno-exceptions -DCORO_DISABLE_EXCEPTION -Iinclude main.cpp
```

CMake 选项 `-DCORO_DISABLE_EXCEPTION=ON` 会向消费者导出该宏；本项目的测试构建还会关闭编译器异常。
消费者应自行配置编译器和第三方库的异常选项。

未设置此 CMake 选项时，配置过程会探测编译器当前的异常模式：异常不可用时默认设为 `ON`，否则为 `OFF`。
显式缓存选项或父项目变量始终优先，包括显式 `OFF`；因此与编译器状态冲突的显式配置可能导致编译失败。
CMake 会缓存选定值；修改编译器选项后，如需重新自动探测，配置时传入 `-UCORO_DISABLE_EXCEPTION`。
探测使用工具链及 CMake 全局编译选项；若通过目录或目标的编译选项控制异常，请显式设置此选项，
并保证所有编译单元配置一致。

### 协程帧内存与 MCU 部署

本库不内置独立的内存管理器。协程帧默认通过全局 `operator new/delete` 分配和回收，
应用已替换这些入口时，可直接复用现有内存管理，`async<T>` 用法不变。
这样既避免重复维护内存池，也不为一般场景增加额外配置和管理开销；MCU 应用可按容量和实时性要求选择分配策略。

RTOS 任务所需的栈上限同样需要分析和运行验证，为每个任务独立预留的余量可能长期闲置。
协程帧按生命周期统一分配和回收，让先后执行的任务复用存储，有机会提高有限 RAM 的利用率，
也便于组合嵌套调用和临时并发任务；但仍需预算峰值占用、分配耗时和耗尽行为。

- **MCU 也需要运行时分配支持**，但不必使用系统堆：可以由应用已有的静态 RAM 池提供。
- **容量需要按同时存活的帧估算**。单个帧大小由编译器决定，父子协程及内部辅助协程可能同时占用内存。
  并发和嵌套深度受限时，可以预先确定预算，再通过目标平台测量验证；不能只看 `sizeof(async<T>)`。
  协程执行时仍需要线程调用栈，也应计入内存预算。
- **动态分配不必然带来不可控碎片**。固定块池可避免池内外部碎片，但仍有块内浪费和容量耗尽；
  可变大小分配器不能普遍保证无碎片。应按业务负载选择策略并留出余量。
- **无异常不等于无分配**。`CORO_DISABLE_EXCEPTION` 不会让内存不足自动返回空任务；
  关闭编译器异常时，应由平台或分配器明确终止、复位等耗尽处理，不能让通常应抛异常的 `operator new` 返回空指针。

执行器队列、channel、组合器共享状态及部分调试设施等也可能分配内存。
MCU 部署需评估整条调用链的内存需求，并保证分配器活到所有相关对象销毁之后。

### 调试协程泄漏

日志钩子在调用线程上执行，可能被并发调用，请使用线程安全的日志函数。泄漏数量是记录对应事件时的快照。

`CORO_DEBUG_LIFECYCLE` 和 `CORO_DEBUG_LEAK_LOG` 的日志钩子在所有配置下都不得抛异常，包括协程创建、析构和显式 dump。
日志实现如可能失败，应在钩子内部处理；库不提供日志异常的传播、忽略或恢复保证。

涉及分配、执行器提交、用户值构造或可能失败的互斥锁加锁操作，通常不能声明为 `noexcept`。
加锁的就绪检查和查询操作根据互斥锁类型使用条件 `noexcept`，保留 `_st` 版本的不抛异常保证。
最终挂起和析构必须保持不抛异常；此时无法投递续体属于不可恢复错误，用户值的析构函数和内部互斥锁的 `unlock()` 不应抛异常。
启用异常处理时，条件变量另行处理允许解锁失败的兼容用户互斥锁：移除等待者，或等待正在进行的通知完成交接后，再在 `co_await` 处重新抛出异常。

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
读取源码字符串。映像字符串每次最多读取 4096 字节，过长的字符串以 `...` 标记截断。
普通 Breakpad minidump 可能缺少必要的协程帧内存，本功能并非
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

### WebAssembly

CI 使用 Emscripten 4.0.23 和 Node.js 运行 13 个示例及核心 move-only 回归测试，
覆盖异常传播、协程泄漏检查，以及 Debug/Release 和异常开关组合。
测试范围为单线程 WebAssembly 执行，不包含浏览器集成或 WebAssembly pthreads。
激活 Emscripten SDK 后可运行：

```bash
emcmake cmake -S test/cmake/wasm -B build_wasm -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-fexceptions -DCORO_DISABLE_EXCEPTION=OFF
cmake --build build_wasm -j 2
ctest --test-dir build_wasm --output-on-failure
```

无异常配置请在独立构建目录中使用 `-DCMAKE_CXX_FLAGS=-fno-exceptions -DCORO_DISABLE_EXCEPTION=ON`。

### CMake 选项

| 选项                             | 默认值         | 说明                  |
|--------------------------------|-------------|---------------------|
| `CORO_BUILD_TEST`              | ON (当作为主项目) | 构建测试                |
| `CORO_BUILD_EXAMPLES` | 作为主项目时 ON | 构建最小可运行示例 |
| `CORO_WARNINGS_AS_ERRORS`      | 构建测试或示例时 ON | 将项目构建警告视为错误       |
| `CORO_TEST_RUNNER_VERY_SLOW`   | OFF         | 为慢速测试环境放宽时间限制      |
| `CORO_ENABLE_SANITIZE_ADDRESS` | OFF         | 启用 AddressSanitizer |
| `CORO_ENABLE_SANITIZE_THREAD`  | OFF         | 启用 ThreadSanitizer  |
| `CORO_DISABLE_EXCEPTION`       | 未设置时自动探测 | 禁用异常支持              |
| `CORO_DEBUG_ASYNC_STACK`       | OFF         | 启用异步调用链诊断信息 |

macOS GitHub Actions runner 会自动启用 `CORO_TEST_RUNNER_VERY_SLOW`。

库头文件、项目测试和 CMake 消费端回归测试必须在 CI 矩阵中保持零编译警告。警告视为错误的选项仅作用于
项目测试，不通过库接口传播给消费者。ASIO 头文件作为外部系统头文件处理；GCC TSan 无法插桩 ASIO 的
`atomic_thread_fence`，仅在 ASIO 测试目标使用 `-Wno-tsan` 处理这一已知工具链限制，保留内存屏障和 sanitizer 插桩。
GCC 15 MinGW 的 ASIO 测试另行禁用内联，避开其协程帧清理诊断，同时保留释放后使用的警告检查。
这些测试设置不会影响库的消费者。

## 项目结构

```
coro/
├── examples/                 # 最小可运行示例
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
