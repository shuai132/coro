# 协作式停止与收尾

[English](cooperative-stop.md) · [返回 README](../README_CN.md)

协作式停止分为两步：用 `event` 通知任务停止，用 `wait_group` 等待任务完成收尾。
`event.set()` 会通知所有当前等待者，并让后来的等待者直接通过；任务也可以用 `is_set()` 主动检查。
任务负责响应通知并主动返回；停止事件不会强制终止协程或自动中断其他等待操作。

下面的完整示例包含两个分段执行的工作任务，以及一个等待停止事件的任务。
`when_any` 返回后，管理协程通知剩余任务停止，并等待全部收尾；最后才停止执行器。

```cpp
#include <cstdio>
#include <exception>
#include <utility>

#include "coro.hpp"

using namespace coro;
using namespace std::chrono_literals;

// 即使任务抛出异常，也在其退出时通知完成。
struct completion_guard {
    wait_group_st& finished;
    ~completion_guard() { finished.done(); }
};

async<void> tracked(async<void> task, wait_group_st& finished) {
    completion_guard guard{finished};
    co_await std::move(task);
}

async<void> worker(event_st& stop, const char* name, int steps) {
    for (int i = 0; i < steps; ++i) {
        if (stop.is_set()) {
            break;
        }
        co_await sleep(20ms); // 定时等待本身不会被 stop 中断。
        if (stop.is_set()) {
            break;
        }
        std::printf("%s: step %d\n", name, i + 1);
    }
    std::printf("%s: cleanup complete\n", name);
}

async<void> stop_observer(event_st& stop) {
    co_await stop.wait();
    std::puts("observer: stop received, cleanup complete");
}

async<void> example() {
    event_st stop;
    wait_group_st finished;
    finished.add(3); // 在启动任何任务前登记。

#ifndef CORO_DISABLE_EXCEPTION
    std::exception_ptr failure;
    try {
#endif
        // 短任务先完成；其余任务仍然存活。
        (void)co_await when_any(
            tracked(worker(stop, "short", 3), finished),
            tracked(worker(stop, "long", 100), finished),
            tracked(stop_observer(stop), finished));
#ifndef CORO_DISABLE_EXCEPTION
    } catch (...) {
        failure = std::current_exception();
    }
#endif

    stop.set();              // 广播停止请求，不表示任务已经结束。
    co_await finished.wait(); // 保持 stop、finished 和共享资源存活。
    std::puts("all tasks finished; shared resources can be released");
#ifndef CORO_DISABLE_EXCEPTION
    if (failure) {
        std::puts("the first completed task failed; shutdown is complete");
    }
#endif
}

int main() {
    executor_loop executor;
    example().detach(executor, [&executor] {
        executor.stop(); // 业务任务退出后才停止执行器。
    });
    executor.run_loop(); // 只在这个线程运行，适用 _st 原语。
    return 0;
}
```

在库根目录将示例保存为 `stop_example.cpp`，可编译运行：

```sh
c++ -std=c++20 -O2 -pthread -I include stop_example.cpp -o stop_example
./stop_example
```

也可在编译命令中添加 `-DCORO_DISABLE_EXCEPTION -fno-exceptions` 验证无异常模式。
本例仅在一个线程调用 `run_loop()`，因此使用 `_st` 原语。若由其他线程直接操作这些对象，应使用默认类型或 `_mt`；
也可以通过线程安全的投递接口，将停止请求交给运行这些任务的线程处理。

## when_any 返回后的收尾

- `when_any` 只报告首个完成结果，不会取消或等待其他分支。示例为每个分支登记完成计数，
  并在子任务退出后通过守卫调用 `done()`；启用异常时，首个分支抛出异常也会先通知停止并等待收尾。
  示例仅报告 `when_any` 传播的错误；如果需要收集所有分支错误，应另行记录每个分支的结果。
- 停止通知与完成确认是两件事：调用 `stop.set()` 后仍需 `co_await finished.wait()`，再销毁任务引用的资源。
  此时执行器仍必须运行；不要提前调用 `executor.stop()`。示例不承诺从内存分配或执行器提交失败中恢复。
- `event` 不会中断 `sleep`、channel 接收或外部异步操作。示例的工作任务在当前 20 ms 定时等待结束后检查停止，
  实际响应还取决于执行器调度。对于可能无限等待的操作，需要关闭专用 channel、提交结束消息，或调用外部 API 提供的取消接口并等待操作完成。
- 不要在循环中反复用 `when_any(recv_task(), stop_task())` 模拟 `select` 而不收尾：未完成的分支会继续存活，
  可能积累等待者或消费后续消息。即使内层 `when_any` 已返回，其剩余分支也必须纳入生命周期管理；
  外层任务完成并不代表这些分支已结束。
- 同一轮任务共享的停止事件一旦设置就不要清除。全部任务退出后，再为下一轮创建新的事件和等待组。
