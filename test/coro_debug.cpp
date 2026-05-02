#define CORO_DEBUG_PROMISE_LEAK

#include <cstring>
#include <thread>
#include <vector>

#include "assert_def.h"
#include "coro/coro.hpp"
#include "coro/executor_loop.hpp"
#include "coro/when.hpp"
#include "log.h"
#include "utils.hpp"

using namespace coro;

std::coroutine_handle<> debug_leaf_handle;

struct pause_awaitable {
  bool await_ready() const noexcept {
    return false;
  }
  void await_suspend(std::coroutine_handle<> handle) noexcept {
    debug_leaf_handle = handle;
  }
  void await_resume() const noexcept {}
};

async<int> debug_leaf() {
  co_await pause_awaitable{};
  co_return 42;
}

async<int> debug_middle() {
  co_return co_await debug_leaf();
}

async<void> debug_root(int& result) {
  result = co_await debug_middle();
}

async<int> debug_immediate() {
  co_return 7;
}

async<void> debug_all(int& result) {
  auto values = co_await when_all(debug_leaf(), debug_immediate());
  result = std::get<0>(values) + std::get<1>(values);
}

async<void> debug_any() {
  co_await when_any(debug_leaf(), debug_immediate());
}

#ifndef CORO_DISABLE_EXCEPTION
async<void> debug_throw() {
  throw 42;
  co_return;
}

async<void> debug_catch(bool& caught) {
  try {
    co_await debug_throw();
  } catch (int value) {
    caught = value == 42;
  }
}
#endif

// Stable breakpoint for debugger integration tests, with all three frames alive.
extern "C" void coro_debug_checkpoint() {
  ASSERT(debug_leaf_handle);
}

int main() {
  ASSERT(detail::async_debug_registry::head_ == nullptr);
  {
    auto lazy = debug_immediate();
    auto id = lazy.current_coro_handle_.promise().debug_node_.id_;
    auto moved = std::move(lazy);
    ASSERT(moved.current_coro_handle_.promise().debug_node_.id_ == id);
    moved.current_coro_handle_.resume();
    ASSERT(moved.await_resume() == 7);
    ASSERT(moved.current_coro_handle_.promise().debug_node_.completed_);
  }
  ASSERT(detail::async_debug_registry::head_ == nullptr);
  {
    int result = 0;
    auto root = debug_root(result);
    root.current_coro_handle_.resume();
    coro_debug_checkpoint();
    auto* leaf = detail::async_debug_registry::head_;
    ASSERT(leaf->frame_ == debug_leaf_handle.address());
    ASSERT(std::strstr(leaf->function_, "debug_leaf"));
    ASSERT(std::strstr(leaf->file_, "coro_debug.cpp"));
    ASSERT(leaf->line_ > 0);
    ASSERT(leaf->parent_id_ == leaf->next_->id_);
    ASSERT(leaf->next_->parent_id_ == root.current_coro_handle_.promise().debug_node_.id_);
    ASSERT(root.current_coro_handle_.promise().debug_node_.parent_id_ == 0);
    std::thread resume_thread([] {
      debug_leaf_handle.resume();
    });
    resume_thread.join();
    ASSERT(root.current_coro_handle_.done());
    ASSERT(result == 42);
    ASSERT(detail::async_debug_registry::head_ == &root.current_coro_handle_.promise().debug_node_);
  }
  ASSERT(detail::async_debug_registry::head_ == nullptr);
  {
    // Destroy a never-started frame and replace another via move assignment.
    auto lazy = debug_immediate();
    auto other = debug_immediate();
    auto id = other.current_coro_handle_.promise().debug_node_.id_;
    lazy = std::move(other);
    ASSERT(detail::async_debug_registry::head_->id_ == id);
    ASSERT(detail::async_debug_registry::head_->next_ == nullptr);
  }
  {
    executor_loop loop;
    int result = 0;
    debug_all(result).detach_with_callback(loop, [&loop] {
      loop.stop();
    });
    loop.post([&loop] {
      auto* leaf = detail::async_debug_registry::head_;
      while (leaf && leaf->frame_ != debug_leaf_handle.address()) {
        leaf = leaf->next_;
      }
      ASSERT(leaf);
      // leaf -> detached when_all_task -> when_all_impl -> debug_all -> callback wrapper
      int depth = 0;
      while (leaf) {
        ++depth;
        auto parent = leaf->parent_id_;
        leaf = detail::async_debug_registry::head_;
        while (leaf && leaf->id_ != parent) {
          leaf = leaf->next_;
        }
      }
      ASSERT(depth == 5);
      loop.post([] {
        debug_leaf_handle.resume();
      });
    });
    loop.run_loop();
    ASSERT(result == 49);
  }
  ASSERT(detail::async_debug_registry::head_ == nullptr);
  {
    executor_loop loop;
    debug_any().detach_with_callback(loop, [] {});
    loop.post([&loop] {
      // The winning branch has destroyed when_any_impl; the loser is still alive.
      auto* node = detail::async_debug_registry::head_;
      while (node && node->frame_ != debug_leaf_handle.address()) {
        node = node->next_;
      }
      ASSERT(node);
      auto* wrapper = detail::async_debug_registry::head_;
      while (wrapper && wrapper->id_ != node->parent_id_) {
        wrapper = wrapper->next_;
      }
      ASSERT(wrapper);
      ASSERT(wrapper->parent_id_ != 0);
      for (auto* live = detail::async_debug_registry::head_; live; live = live->next_) {
        ASSERT(live->id_ != wrapper->parent_id_);
      }
      debug_leaf_handle.resume();
      loop.stop();
    });
    loop.run_loop();
  }
#ifndef CORO_DISABLE_EXCEPTION
  {
    bool caught = false;
    auto task = debug_catch(caught);
    task.current_coro_handle_.resume();
    ASSERT(caught);
    ASSERT(task.current_coro_handle_.promise().debug_node_.completed_);
    ASSERT(detail::async_debug_registry::head_->next_ == nullptr);
  }
#endif
  ASSERT(detail::async_debug_registry::head_ == nullptr);
  std::vector<std::thread> workers;
  for (int i = 0; i < 4; ++i) {
    workers.emplace_back([] {
      for (int n = 0; n < 100; ++n) {
        auto task = debug_immediate();
        task.current_coro_handle_.resume();
        ASSERT(task.await_resume() == 7);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  ASSERT(detail::async_debug_registry::head_ == nullptr);
  check_coro_leak();
}
