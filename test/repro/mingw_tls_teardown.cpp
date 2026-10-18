// Standalone MinGW emutls reproducer; see README.md in this directory.
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

int main() {
  static int completed = 0;
  static int entered = 0;
  std::thread worker([] {
    struct late_task {
#ifndef CORO_REPRO_STATELESS_GUARD
      int& completed_;
      int& entered_;
#endif
      ~late_task() {
#ifdef CORO_REPRO_STATELESS_GUARD
        entered = 1;
#else
        entered_ = 1;
#endif
        std::vector<std::unique_ptr<std::function<void()>>> callbacks;
        for (int i = 0; i < 64; ++i) {
          callbacks.push_back(std::make_unique<std::function<void()>>([] {}));
        }
#ifdef CORO_REPRO_STATELESS_GUARD
        completed = 42;
#else
        completed_ = 42;
#endif
      }
    };
#ifdef CORO_REPRO_STATELESS_GUARD
    thread_local late_task late;
#else
    thread_local late_task late{completed, entered};
#endif
    (void)late;
  });
  worker.join();
  std::printf("entered=%d completed=%d\n", entered, completed);
  return entered != 1 || completed != 42;
}
