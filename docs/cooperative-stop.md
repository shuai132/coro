# Cooperative Stop and Cleanup

[中文](cooperative-stop_CN.md) · [Back to README](../README.md)

Cooperative shutdown has two steps: use an `event` to request stop, then use a `wait_group` to wait for task cleanup.
`event.set()` notifies all current waiters and lets later waiters proceed immediately;
tasks can also check `is_set()`. Each task responds to the request and returns voluntarily. The event does not
forcibly terminate coroutines or automatically interrupt unrelated waits.

The following complete example runs two workers in bounded steps and a third task waiting for the stop event.
After `when_any` returns, the coordinator requests that the remaining tasks stop and waits for cleanup before
stopping the executor.

```cpp
#include <cstdio>
#include <exception>
#include <utility>

#include "coro.hpp"

using namespace coro;
using namespace std::chrono_literals;

// Notify completion when the task exits, including exceptional exits.
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
        co_await sleep(20ms); // The timer itself is not interrupted by stop.
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
    finished.add(3); // Register before starting any task.

#ifndef CORO_DISABLE_EXCEPTION
    std::exception_ptr failure;
    try {
#endif
        // The short task finishes first; the other tasks remain alive.
        (void)co_await when_any(
            tracked(worker(stop, "short", 3), finished),
            tracked(worker(stop, "long", 100), finished),
            tracked(stop_observer(stop), finished));
#ifndef CORO_DISABLE_EXCEPTION
    } catch (...) {
        failure = std::current_exception();
    }
#endif

    stop.set();              // Broadcast a stop request; tasks may still be running.
    co_await finished.wait(); // Keep stop, finished, and shared resources alive.
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
        executor.stop(); // Stop the executor only after application tasks exit.
    });
    executor.run_loop(); // Run on this thread only, as required by the _st primitives.
    return 0;
}
```

Save the example as `stop_example.cpp` in the library root, then build and run it:

```sh
c++ -std=c++20 -O2 -pthread -I include stop_example.cpp -o stop_example
./stop_example
```

Add `-DCORO_DISABLE_EXCEPTION -fno-exceptions` to the compiler command to check the no-exception mode.
This example calls `run_loop()` on one thread only, so it uses `_st` primitives. If other threads access these objects
directly, use the default types or `_mt` variants. Alternatively, use a thread-safe submission API to post stop requests
to the thread running these tasks.

## Cleanup After when_any Returns

- `when_any` reports the first completion without cancelling or joining other branches. The example registers each
  branch and uses a guard to call `done()` after its child task exits. With exceptions enabled, a failure propagated
  by `when_any` also leads to a stop request and cleanup. The example only reports that propagated failure;
  record each branch's result separately if all errors must be collected.
- Requesting stop and confirming completion are separate operations. After `stop.set()`, use
  `co_await finished.wait()` before destroying resources referenced by the tasks. Keep the executor running during
  cleanup; do not call `executor.stop()` early. The example does not promise recovery from allocation or executor
  submission failures.
- An `event` does not interrupt `sleep`, channel receives, or external asynchronous operations. These workers check
  for stop after their current 20 ms timer completes; actual response also depends on executor scheduling. Unbounded waits
  need an exit path, such as closing a dedicated channel, sending an end message, or calling an external API
  to request cancellation and waiting for the operation to finish.
- Do not repeatedly use `when_any(recv_task(), stop_task())` in a loop as a replacement for `select` without cleanup.
  Unfinished branches remain alive and can accumulate waiters or consume later messages. An inner `when_any`'s
  remaining branches must also be tracked: completion of the outer task does not mean those branches have exited.
- Do not clear the shared stop event during the same task lifecycle. Once all tasks have exited, create a fresh
  event and wait group for the next lifecycle.
