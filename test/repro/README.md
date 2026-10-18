# MinGW TLS teardown reproducer

`mingw_tls_teardown.cpp` uses only the C++ standard library. It isolates the
failure behind the `completed == 42` assertion in `coro_core_lifetime`.

From the repository root, with a MinGW GCC toolchain (commands shown for Bash):

```bash
g++ -std=c++20 -O0 -g -Wall -Wextra -Werror -pthread \
  test/repro/mingw_tls_teardown.cpp -o tls_teardown.exe
./tls_teardown.exe

g++ -std=c++20 -O0 -g -Wall -Wextra -Werror -pthread \
  -DCORO_REPRO_STATELESS_GUARD \
  test/repro/mingw_tls_teardown.cpp -o tls_teardown_stateless.exe
./tls_teardown_stateless.exe
```

Both programs should print `entered=1 completed=42` and exit with code zero.
The first intentionally retains the original member-bearing TLS guard to expose
the toolchain defect. The second uses the stateless guard adopted by the test.
This diagnostic is separate from the normal suite because its first mode
reproduces an upstream runtime failure; the full coroutine teardown regression
remains enabled in CI.

## Confirmed behavior

With WinLibs GCC 15.2.0, UCRT/POSIX/SEH r3 (MinGW-w64 13.0.0), under Wine 11.0:

- Original reproducer: `entered=1 completed=0`, exit code 1.
- Stateless guard: `entered=1 completed=42`, exit code 0.
- Original CI commit's `coro_core_lifetime`: fails at `completed == 42`.
- Patched `coro_core_lifetime`: passes, including callback and coroutine leak checks.

The corresponding coro diagnostic also fails with WinLibs GCC 15.2.0 r7
(MinGW-w64 14.0.0). A Homebrew cross-built GCC 16.2.0 toolchain passes it, but
this does not establish that upgrading GCC alone fixes the defect: its TLS
destructor registration uses a different runtime implementation.

## Cause and scope of the workaround

Affected MinGW emutls runtimes release the storage of a `thread_local` object
before running its C++ destructor. Allocations in that destructor can reuse the
storage and overwrite its reference members. In the coro diagnostic, the
references change to invalid addresses after posting a callback. Wine's SEH
trace records an access violation on the next member access, followed by the
thread-exit callback's exception handling; the final completion assertion then
reports the failure.

Instrumenting the standalone reproducer confirms that its second callback
allocation overlaps the TLS guard's storage while its destructor is still
executing. Ten consecutive runs fail in the original mode and pass in the
stateless mode.

This matches the upstream reports
[MinGW-w64 #727](https://sourceforge.net/p/mingw-w64/bugs/727/) and
[MSYS2 #2519](https://github.com/msys2/MINGW-packages/issues/2519).

The test now keeps its completion flag in static storage and its TLS guard has
no members. No test state is read through the guard after teardown allocations.
The flag is reset before starting the worker, checked to remain zero in the
thread body, and checked to equal 42 after joining. Callback execution, cache
shutdown ordering, coroutine execution, and leak assertions are preserved.
This is a test-fixture workaround; it does not repair the upstream runtime for
applications that use member-bearing TLS destructors.
