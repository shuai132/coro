# AGENTS.md

AI 编程助手在本 C++20 协程库中的开发指南。

## 项目概述

**coro** 是一个轻量级、仅头文件的 C++20 协程库，提供异步任务、同步原语和并发控制。设计目标：简洁、跨平台（包括嵌入式/MCU）、可选的无异常模式。

## 构建命令

```bash
# 标准构建
cmake -S . -B build && cmake --build build -j

# 启用 AddressSanitizer
cmake -S . -B build -DCORO_ENABLE_SANITIZE_ADDRESS=ON && cmake --build build

# 启用 ThreadSanitizer
cmake -S . -B build -DCORO_ENABLE_SANITIZE_THREAD=ON && cmake --build build

# 禁用异常（用于嵌入式平台）
cmake -S . -B build -DCORO_DISABLE_EXCEPTION=ON && cmake --build build
```

默认使用`build`文件夹，特殊构建时，可使用`build_asan`，`build_release`文件夹。

## 运行测试

每个测试是独立的可执行文件，在 build 目录下运行：

```bash
./coro_task              # 核心异步任务测试
./coro_mutex             # 互斥锁测试
./coro_channel           # 通道测试
./coro_when              # when_all/when_any 测试
./coro_semaphore         # 信号量测试
./coro_condition_variable    # 条件变量测试
./coro_wait_group        # 等待组测试
./coro_latch             # 闩锁测试
./coro_event             # 事件测试
./coro_broadcast         # 通道广播测试
./coro_task_verbose      # 带生命周期日志的任务测试
./coro_multi_thread      # 多线程测试
./coro_multi_thread_st   # 多线程测试（单线程模式）
```

**无测试框架** - 测试使用自定义 `ASSERT()` 宏和 `LOG()` 输出。测试通过的标志是退出码为 0。

## 代码风格

### 格式化

- **clang-format**：基于 Google 风格，150 列宽限制
- 提交前运行：`clang-format -i <文件>`
- 短函数/lambda：仅空函数体可单行

### 命名规范

- **命名空间**：`coro`，内部实现用 `coro::detail`
- **类型**：`snake_case`（如 `mutex_t`、`awaitable_promise`、`counting_semaphore_t`）
- **类型别名**：`snake_case`（如 `using mutex = mutex_t;`）
- **函数/方法**：`snake_case`（如 `scoped_lock()`、`await_ready()`）
- **成员变量**：`snake_case_` 带尾部下划线（如 `counter_`、`mutex_`）
- **模板参数**：`UPPER_CASE`（如 `MUTEX`、`T`）
- **宏**：`UPPER_CASE` 带前缀（如 `CORO_DEBUG_PROMISE_LEAK`）

### 头文件包含顺序

1. 配置/调试宏（在库头文件之前）
2. 标准库头文件（`<coroutine>`、`<atomic>` 等）
3. 项目头文件（`"coro/coro.hpp"`、`"coro/executor.hpp"`）

### 类型设计模式

- **模板互斥锁参数**：使用 `typename MUTEX = std::mutex` 控制线程安全
- **类型别名**：提供 `_mt`（多线程）和 `_st`（单线程）变体
- **可等待类型**：嵌套结构体命名为 `*_awaitable`（如 `lock_awaitable`、`send_awaitable`）

### 协程模式

- 返回类型：`async<T>`（`awaitable<T>` 的别名）
- 使用 `co_await`、`co_return` 关键字
- 延迟启动：协程在 `initial_suspend()` 处挂起
- 执行器继承：子协程继承父协程的执行器
- **协程 lambda 禁止捕获引用**：lambda 立即调用后销毁，捕获的引用会悬空，需用参数传递

```cpp
async<int> example_coro() {
  co_await sleep(100ms);
  co_return 42;
}

// 错误：lambda 销毁后捕获的引用悬空
spawn(exec, [&counter]() -> async<void> {
  counter++;  // 未定义行为
}());

// 正确：通过参数传递
spawn(exec, [](int& cnt) -> async<void> {
  cnt++;
}(counter));
```

### 错误处理

- **启用异常**（默认）：在 promise 中使用 `std::exception_ptr`
- **禁用异常**（`CORO_DISABLE_EXCEPTION`）：返回值使用 `std::optional`
- 两种路径都需要用 `#ifndef CORO_DISABLE_EXCEPTION` 保护

### 内存与生命周期

- 禁用拷贝，启用移动
- 使用 RAII 守卫（如 `lock_guard`）管理资源
- 使用侵入式链表管理等待队列（避免堆分配）

### 测试文件结构

```cpp
#define CORO_DEBUG_PROMISE_LEAK
#include "log.h"
#include "TimeCount.hpp"
#include "assert_def.h"
#include "coro/coro.hpp"
#include "utils.hpp"

using namespace coro;

async<void> test_feature() {
  ASSERT(condition);
  LOG("Test passed");
}

int main() {
  LOG("Test init");
  executor_loop executor;
  test_feature().detach_with_callback(executor, [&] {
    executor.stop();
  });
  executor.run_loop();
  check_coro_leak();  // 验证无协程泄漏
  return 0;
}
```

## 核心 API

| 组件                           | 用法                          |
|------------------------------|-----------------------------|
| `async<T>`                   | 协程返回类型                      |
| `spawn(executor, coro)`      | 启动分离的协程                     |
| `co_await mtx.scoped_lock()` | RAII 互斥锁                    |
| `co_await ch.send(val)`      | 通道发送                        |
| `co_await ch.recv()`         | 通道接收（返回 `std::optional<T>`） |
| `co_await sem.acquire()`     | 信号量获取                       |
| `sem.release()`              | 信号量释放（非阻塞）                  |
| `when_all(...)`              | 等待所有可等待对象                   |
| `when_any(...)`              | 等待任意一个完成                    |

## 调试宏

```cpp
#define CORO_DEBUG_PROMISE_LEAK      // 启用泄漏追踪
#define CORO_DEBUG_LEAK_LOG LOG      // 泄漏日志函数
#define CORO_DEBUG_LIFECYCLE LOG     // 协程生命周期日志
```

测试结束时调用 `check_coro_leak()` 验证无泄漏。

## 添加新功能

添加新功能时需要同时更新三个文件：

### 1. CMakeLists.txt

在 `if (CORO_BUILD_TEST)` 块中添加：

```cmake
set(TARGET_NAME ${PROJECT_NAME}_new_feature)
add_executable(${TARGET_NAME} test/coro_new_feature.cpp)
```

### 2. .github/workflows/ci.yml

在测试步骤部分添加：

```yaml
- name: Test new_feature
  if: always()
  working-directory: build
  run: |
    ./coro_new_feature${{ matrix.env.BIN_SUFFIX }}
```

### 3. 创建测试文件 test/coro_new_feature.cpp

使用上述"测试文件结构"模板。

### 4. 补充文档，更新README.md和README_CN.md，放到同类功能后面

## 平台说明

- **编译器**：GCC、Clang、MSVC（需 `/Zc:preprocessor`）
- **Windows**：添加 `-DNOMINMAX` 避免宏冲突
- **嵌入式**：定义 `CORO_DISABLE_EXCEPTION` 用于不支持异常的平台
