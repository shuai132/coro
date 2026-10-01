#pragma once

#ifdef CORO_DEBUG_ASYNC_STACK
#include <atomic>
#include <cstdint>
#include <mutex>
#if __has_include(<source_location>)
#include <source_location>
#endif

namespace coro::detail {

#if !defined(__cpp_lib_source_location) || (defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 14)
// Some coroutine-capable libraries lack source_location; older GCC also rejects its consteval default argument.
struct async_debug_location {
  const char* function_;
  const char* file_;
  std::uint_least32_t line_;

  static constexpr async_debug_location current(const char* function = __builtin_FUNCTION(), const char* file = __builtin_FILE(),
                                                std::uint_least32_t line = __builtin_LINE()) noexcept {
    return {function, file, line};
  }
  constexpr const char* function_name() const noexcept {
    return function_;
  }
  constexpr const char* file_name() const noexcept {
    return file_;
  }
  constexpr std::uint_least32_t line() const noexcept {
    return line_;
  }
};
#else
using async_debug_location = std::source_location;
#endif

struct async_debug_node;

// Plain fields form the debugger interface; scripts read them only with all threads stopped.
struct async_debug_registry {
  static std::mutex& mutex() {
    // Global destructors in other translation units can still create and destroy coroutine frames.
    static auto* value = new std::mutex;
    return *value;
  }
  inline static async_debug_node* head_ = nullptr;
  inline static std::uint64_t next_id_ = 0;
  inline static volatile bool updating_ = false;
};

struct async_debug_guard {
  std::lock_guard<std::mutex> lock_{async_debug_registry::mutex()};

  async_debug_guard() {
    async_debug_registry::updating_ = true;
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }
  ~async_debug_guard() {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    async_debug_registry::updating_ = false;
  }
};

struct async_debug_node {
  async_debug_node* next_ = nullptr;
  async_debug_node* prev_ = nullptr;
  void* frame_ = nullptr;
  std::uint64_t id_ = 0;
  std::uint64_t parent_id_ = 0;
  void* external_parent_ = nullptr;
  const char* function_ = "";
  const char* file_ = "";
  std::uint_least32_t line_ = 0;
  bool completed_ = false;

  async_debug_node() {
    // Promise construction reliably unwinds its bases if allocation fails; some coroutine backends
    // omit promise destruction when get_return_object throws.
    (void)async_debug_registry::mutex();
  }
  async_debug_node(const async_debug_node&) = delete;
  async_debug_node& operator=(const async_debug_node&) = delete;

  void register_frame(void* frame, async_debug_location location) {
    async_debug_guard lock;
    frame_ = frame;
    id_ = ++async_debug_registry::next_id_;
    function_ = location.function_name();
    file_ = location.file_name();
    line_ = location.line();
    next_ = async_debug_registry::head_;
    if (next_) {
      next_->prev_ = this;
    }
    async_debug_registry::head_ = this;
  }

  void set_parent(std::uint64_t id, void* external = nullptr) noexcept {
    async_debug_guard lock;
    parent_id_ = id;
    external_parent_ = external;
  }

  void complete() noexcept {
    async_debug_guard lock;
    completed_ = true;
    parent_id_ = 0;
    external_parent_ = nullptr;
  }

  ~async_debug_node() {
    if (!frame_) {
      return;
    }
    async_debug_guard lock;
    if (prev_) {
      prev_->next_ = next_;
    } else {
      async_debug_registry::head_ = next_;
    }
    if (next_) {
      next_->prev_ = prev_;
    }
  }
};

}  // namespace coro::detail
#endif
