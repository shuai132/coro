#pragma once

// Linked into the allocation-measurement executable only. All workloads are single-threaded.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace allocation_counter {
struct statistics {
  std::size_t calls = 0;
  std::size_t requested = 0;
  std::size_t live = 0;
  std::size_t peak = 0;
};
inline thread_local statistics stats;
inline thread_local std::size_t epoch = 0;
inline thread_local bool enabled = false;

struct header {
  void* base;
  std::size_t size;
  std::size_t epoch;
};

inline void begin() {
  stats = {};
  ++epoch;
  enabled = true;
}

inline statistics end() {
  enabled = false;
  return stats;
}

inline void* allocate(std::size_t size, std::size_t alignment) {
  alignment = alignment < alignof(header) ? alignof(header) : alignment;
  const auto padding = sizeof(header) + alignment - 1;
  const auto storage_size = size == 0 ? 1 : size;
  if (storage_size > std::numeric_limits<std::size_t>::max() - padding) {
    throw std::bad_alloc();
  }
  void* base = std::malloc(storage_size + padding);
  if (!base) {
    throw std::bad_alloc();
  }
  const auto start = reinterpret_cast<std::uintptr_t>(base) + sizeof(header);
  const auto address = (start + alignment - 1) & ~(alignment - 1);
  auto* metadata = reinterpret_cast<header*>(address) - 1;
  new (metadata) header{base, size, enabled ? epoch : 0};
  if (enabled) {
    ++stats.calls;
    stats.requested += size;
    stats.live += size;
    if (stats.live > stats.peak) {
      stats.peak = stats.live;
    }
  }
  return reinterpret_cast<void*>(address);
}

inline void release(void* pointer) noexcept {
  if (!pointer) {
    return;
  }
  auto* metadata = static_cast<header*>(pointer) - 1;
  if (enabled && metadata->epoch == epoch) {
    stats.live -= metadata->size;
  }
  std::free(metadata->base);
}
}  // namespace allocation_counter

void* operator new(std::size_t size) {
  return allocation_counter::allocate(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void* operator new[](std::size_t size) {
  return ::operator new(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocation_counter::allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void operator delete(void* pointer) noexcept {
  allocation_counter::release(pointer);
}
void operator delete[](void* pointer) noexcept {
  ::operator delete(pointer);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
  ::operator delete(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
  ::operator delete(pointer);
}
#ifdef __cpp_sized_deallocation
void operator delete(void* pointer, std::size_t) noexcept {
  ::operator delete(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
  ::operator delete(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
  ::operator delete(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  ::operator delete(pointer);
}
#endif
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
  return ::operator new(size, tag);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& tag) noexcept {
  return ::operator new(size, alignment, tag);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  ::operator delete(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  ::operator delete(pointer);
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  ::operator delete(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  ::operator delete(pointer);
}
