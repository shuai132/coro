#pragma once

#include <functional>
#include <memory>
#include <utility>

namespace coro::detail {

// Empty callback objects only; captured resources are released before caching.
struct callback_cache_state {
  std::function<void()>* slots_[2]{};
  bool available_ = true;
  bool cleanup_registered_ = false;
};
inline constinit thread_local callback_cache_state cached_callbacks;

inline void register_callback_cache_cleanup() {
  struct cleanup {
    ~cleanup() {
      cached_callbacks.available_ = false;
      for (auto* callback : cached_callbacks.slots_) {
        delete callback;
      }
    }
  };
  static thread_local cleanup guard;
  (void)guard;
}

struct recycle_callback {
  void operator()(std::function<void()>* callback) const noexcept {
    *callback = nullptr;
    if (cached_callbacks.available_ && cached_callbacks.cleanup_registered_) {
      for (auto& slot : cached_callbacks.slots_) {
        if (!slot) {
          slot = callback;
          return;
        }
      }
    }
    delete callback;
  }
};

using queued_callback = std::unique_ptr<std::function<void()>, recycle_callback>;

inline queued_callback make_queued_callback(std::function<void()>&& fn) {
  if (cached_callbacks.available_) {
    if (!cached_callbacks.cleanup_registered_) {
      register_callback_cache_cleanup();
      cached_callbacks.cleanup_registered_ = true;
    }
    for (auto& slot : cached_callbacks.slots_) {
      if (slot) {
        queued_callback callback{std::exchange(slot, nullptr)};
        *callback = std::move(fn);
        return callback;
      }
    }
  }
  return queued_callback{new std::function<void()>(std::move(fn))};
}

}  // namespace coro::detail
