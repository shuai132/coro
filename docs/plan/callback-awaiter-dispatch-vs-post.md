# callback_awaiter dispatch vs post analysis

The shutdown observations below describe the earlier drain-on-stop executor. Both built-in executors now stop dequeuing immediately;
`executor_loop` inherits the fixed-thread dispatch semantics of `executor_poll`. Tests must finish losing branches before stopping.

## Question

`callback_awaiter` needs to resume the suspended coroutine after a callback-style API completes. The executor offers two relevant operations:

- `dispatch(fn)`: run `fn` immediately when already on the executor thread, otherwise enqueue through `post()`.
- `post(fn)`: always enqueue, and in the current executor implementation it drops new work after `stop()`.

The original implementation used `dispatch()` directly. During the lifetime fix work, `post()` was briefly tried to prevent re-entrant resume. That avoided one bug but introduced unnecessary scheduling cost and a shutdown leak in `when_any` scenarios. The current implementation uses a hybrid approach.

## Pure dispatch

Pure `dispatch()` is good for the normal asynchronous completion path:

- No extra queue hop when the callback already runs on the executor thread.
- Lower overhead than `post()` in the common timer/executor completion case.
- Allows already-dispatched delayed work to drain during `executor_loop::stop()`, because completion can resume inline from the delayed task already being executed.

However, pure `dispatch()` is unsafe when the callback-style API invokes the completion callback synchronously inside `await_suspend()`:

```cpp
co_await callback_awaiter<int>([](auto callback) {
  callback(123);  // synchronous completion
});
```

If this happens on the executor thread, `dispatch()` calls `handle.resume()` before `await_suspend()` returns. That can destroy the awaiter while `await_suspend()` is still using it, which is re-entrant coroutine resume and is not safe.

## Pure post

Pure `post()` avoids the re-entrant resume because completion always happens later.

The downsides are real:

- It adds an extra queue operation for every callback completion, including callbacks already running on the executor thread.
- It changes executor ordering and latency by forcing an additional turn of the event loop.
- With this executor implementation, `post()` after `stop()` is ignored. This caused no-exception `coro_when` to leak after `when_any` returned: slower tasks reached their delayed callback while the executor was stopping, and their coroutine resumes were dropped.

So pure `post()` is simpler but worse for this library's executor semantics.

## Current decision

Use `dispatch()` after suspension is committed, with one atomic phase shared by registration and completion:

- `await_suspend()` starts in `registering` and invokes the callback-style API.
- The callback stores its result, then atomically exchanges the phase for `completed`.
- After registration returns, `await_suspend()` tries to change `registering` to `suspended`. If completion already won, it returns `false`, allowing the coroutine to continue without suspension.
- If suspension wins, the callback observes `suspended` and resumes through `executor->dispatch(...)`.

The phase change is the handoff point; it does not require `await_suspend()` to have returned. Once suspension is published, `await_suspend()` must not access the awaiter again, because another thread may already resume and destroy it. The atomic operations also publish the stored result before the coroutine reads it.

Separate "registration running" and "completed inline" flags are insufficient: registration can finish while the callback is still storing its result, causing both sides to leave resumption to the other. A single atomic phase ensures exactly one side owns continuation.

This preserves the original low-overhead `dispatch()` behavior for normal asynchronous callbacks while avoiding re-entrant resume for synchronous callbacks.

## Consequences

- The original direct `dispatch()` approach is faster in the happy path but incomplete because it does not handle synchronous callbacks safely.
- Replacing `dispatch()` with `post()` everywhere is not preferred because it adds overhead and can drop resumes during executor shutdown.
- The current hybrid behavior is the intended policy:
  - synchronous callback completion: no re-entrant `resume()`, return `false` from `await_suspend()`;
  - asynchronous callback completion: resume with `dispatch()`.

Regression coverage:

- `coro_core_callback` proves synchronous callback completion does not destroy the awaiter during active callback execution.
- `coro_core_callback` also pauses an asynchronous callback during result storage while registration returns, covering the suspension handoff for both callback API overloads.
- `coro_when` under `CORO_DISABLE_EXCEPTION=ON` proves slower `when_any` branches can still drain after the winner stops the executor.
