# coro.hpp core lifetime and result fixes

## Goals

- Add regression tests that reproduce the observable bugs before changing the implementation.
- Fix coroutine frame ownership without breaking detached tasks.
- Support move-only coroutine return values such as `std::unique_ptr<T>`.
- Avoid synchronous callback re-entry from `callback_awaiter`.
- Keep `CORO_DISABLE_EXCEPTION` builds free of exception-specific callback API requirements where practical.

## Bugs to prove first

1. Destroying an unstarted `async<T>` leaks its coroutine frame.
   - A coroutine starts suspended by `initial_suspend()`.
   - If the returned `awaitable` is never awaited or detached, its destructor currently sees `done() == false` and only clears `awaitable_`.
   - Regression test: create and drop a lazy task with `CORO_DEBUG_PROMISE_LEAK`, then require the leak set to be empty.

2. `callback_awaiter<T>` can resume the awaiting coroutine before `await_suspend()` returns.
   - `executor_loop::dispatch()` runs inline when called from the executor thread.
   - A callback-style API may invoke the completion callback synchronously.
   - Regression test: use a callback functor that records whether it is destroyed while its call operator is still active. The current implementation resumes and destroys the awaiter re-entrantly.

3. `async<T>` does not support move-only return values in exception-enabled builds.
   - `std::variant<std::exception_ptr, T> value_{nullptr}` is not a valid initial state for many `T`.
   - `get_value() const` returns by copy.
   - Regression test: compile and run an `async<std::unique_ptr<int>>` task.

## Fix plan

1. Add tests and CI/CMake targets.
   - Add a runtime core regression test for lazy destruction and callback re-entry.
   - Add a move-only result regression test.
   - Register both in `CMakeLists.txt` and `.github/workflows/ci.yml`.

2. Fix `awaitable` frame ownership.
   - Track whether a coroutine frame has started.
   - Destroy unstarted frames in `awaitable` destruction and move assignment.
   - Preserve detached/running behavior so detached tasks still self-destroy at final suspend.

3. Fix result storage.
   - Use an explicit empty state for exception-enabled promise values.
   - Move non-void values out of the promise in `get_value()`.

4. Fix callback re-entry.
   - Schedule callback completion with `post()` instead of inline-capable `dispatch()`.
   - Keep result assignment immediately before resuming the suspended coroutine.

5. Tighten no-exception callback overloads.
   - Avoid exposing `std::exception_ptr` callback parameters when `CORO_DISABLE_EXCEPTION` is defined.

## Verification

- `cmake -S . -B build && cmake --build build -j`
- `./build/coro_core_lifetime`
- `./build/coro_core_move_only`
- `./build/coro_task`
- Run the full existing test set when the local toolchain is healthy.

## Progress

- Added regression coverage:
  - `test/coro_core_lifetime.cpp`
  - `test/coro_core_callback.cpp`
  - `test/coro_core_move_only.cpp`
- Confirmed pre-fix failures:
  - `coro_core_lifetime` failed with one leaked lazy coroutine frame.
  - `coro_core_callback` failed by observing callback awaiter destruction during an active synchronous callback.
  - `coro_core_move_only` failed to compile because `std::variant<std::exception_ptr, T> value_{nullptr}` does not support `T = std::unique_ptr<int>`.
- Fixed in `include/coro/coro.hpp`:
  - Added started-state tracking for coroutine frames.
  - Destroyed unstarted frames from `awaitable` destruction and move assignment.
  - Switched non-void result retrieval to move values out of promise storage.
  - Replaced the exception-enabled result variant initial state with `std::monostate`.
  - Changed `callback_awaiter` completion scheduling from inline-capable `dispatch()` to `post()`.
  - Removed the exception-handler overload from `with_callback()` and `detach_with_callback()` when `CORO_DISABLE_EXCEPTION` is defined.
- Verified with the local CLT toolchain:
  - Full exception-enabled build passed.
  - Full exception-enabled test suite passed.
  - `CORO_DISABLE_EXCEPTION=ON` build passed.
  - No-exception `coro_core_lifetime`, `coro_core_callback`, `coro_core_move_only`, and `coro_task` passed.
