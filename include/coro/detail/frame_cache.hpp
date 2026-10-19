#pragma once

#include <cstddef>
#include <new>
#include <utility>

#ifndef CORO_FRAME_CACHE_SLOTS
#define CORO_FRAME_CACHE_SLOTS 2
#endif
#ifndef CORO_FRAME_CACHE_MAX_BYTES
#define CORO_FRAME_CACHE_MAX_BYTES 512
#endif

namespace coro::detail {

static_assert(CORO_FRAME_CACHE_SLOTS > 0, "CORO_FRAME_CACHE_SLOTS must be positive");
static_assert(CORO_FRAME_CACHE_MAX_BYTES > 0, "CORO_FRAME_CACHE_MAX_BYTES must be positive");

// Trivial TLS state remains usable by destructors that run after the cleanup guard.
struct frame_cache_state {
  struct slot {
    void* frame_ = nullptr;
    std::size_t size_ = 0;
  };
  slot slots_[CORO_FRAME_CACHE_SLOTS];
  bool available_ = true;
  bool cleanup_registered_ = false;
};
inline constinit thread_local frame_cache_state cached_frames;

inline void register_frame_cache_cleanup() {
  struct cleanup {
    ~cleanup() {
      cached_frames.available_ = false;
      for (auto& entry : cached_frames.slots_) {
        ::operator delete(entry.frame_);
      }
    }
  };
  static thread_local cleanup guard;
  (void)guard;
}

inline void* allocate_frame(std::size_t size, std::size_t alignment) {
  if (alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
    return ::operator new(size, std::align_val_t{alignment});
  }
  if (size <= CORO_FRAME_CACHE_MAX_BYTES && cached_frames.available_) {
    if (!cached_frames.cleanup_registered_) {
      register_frame_cache_cleanup();
      cached_frames.cleanup_registered_ = true;
    }
    bool full = true;
    // Preserve exact-size reuse before borrowing a larger cached allocation.
    frame_cache_state::slot* fitting = nullptr;
    for (auto& entry : cached_frames.slots_) {
      full = full && entry.frame_ != nullptr;
      if (entry.frame_ && entry.size_ == size) {
        return std::exchange(entry.frame_, nullptr);
      }
      if (entry.frame_ && entry.size_ > size && (!fitting || entry.size_ < fitting->size_)) {
        fitting = &entry;
      }
    }
    if (fitting) {
      return std::exchange(fitting->frame_, nullptr);
    }
    // Make room for a new size when all slots are occupied.
    if (full) {
      ::operator delete(std::exchange(cached_frames.slots_[0].frame_, nullptr));
    }
  }
  return ::operator new(size);
}

inline void release_frame(void* frame, std::size_t size, std::size_t alignment) noexcept {
  if (alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
    ::operator delete(frame, std::align_val_t{alignment});
    return;
  }
  if (size <= CORO_FRAME_CACHE_MAX_BYTES && cached_frames.available_ && cached_frames.cleanup_registered_) {
    for (auto& entry : cached_frames.slots_) {
      if (!entry.frame_) {
        // A borrowed larger block still has at least this requested capacity.
        entry = {frame, size};
        return;
      }
    }
  }
  ::operator delete(frame);
}

}  // namespace coro::detail
