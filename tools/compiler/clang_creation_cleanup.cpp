// Standalone compiler probe: clang++ -std=c++20 -O0 -g -fsanitize=address clang_creation_cleanup.cpp -o probe
// Apple Clang 21.0.0 (clang-2100.3.34.2) reports a use-after-free while unwinding the initial-suspend exception.
// This uses only standard C++20 and is intentionally excluded from the library's normal test targets.

#include <coroutine>
#include <exception>

// Keep both creation paths visible to the compiler.
bool fail_initial = true;
struct basic_task {
  struct promise_type {
    basic_task get_return_object() {
      return {};
    }
    std::suspend_always initial_suspend() {
      if (fail_initial) throw 1;
      return {};
    }
    struct final_awaiter {
      bool await_ready() noexcept {
        return false;
      }
      std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
        return std::noop_coroutine();
      }
      void await_resume() noexcept {}
    };
    final_awaiter final_suspend() noexcept {
      return {};
    }
    void return_void() noexcept {}
    void unhandled_exception() noexcept {
      std::terminate();
    }
  };
};
basic_task example(bool fail) {
  // This unexecuted body exception path participates in the affected cleanup lowering.
  if (fail) throw 2;
  co_return;
}
int main() {
  try {
    example(false);
  } catch (int) {
  }
}
