#pragma once

#include <any>
#include <memory>
#include <unordered_map>
#include <utility>

namespace coro {
namespace detail {

// Type-erased storage container for coroutine-local data
// Each coroutine can have its own storage map, and child coroutines
// can optionally inherit from parent coroutines
struct coro_local_map {
  // Keep inherited storage alive until local values have been destroyed.
  std::shared_ptr<coro_local_map> parent;
  std::unordered_map<const void*, std::any> data;

  ~coro_local_map() {
    data.clear();
    // Release exclusively owned ancestors iteratively; shared ancestors keep their links.
    while (parent && parent.use_count() == 1) {
      auto ancestor = std::move(parent);
      parent = std::move(ancestor->parent);
    }
  }

  // Look up a value by key, checking parent chain if not found locally
  template <typename T>
  T* find(const void* key) {
    for (auto* storage = this; storage; storage = storage->parent.get()) {
      auto it = storage->data.find(key);
      if (it != storage->data.end()) {
        return std::any_cast<T>(&it->second);
      }
    }
    return nullptr;
  }

  // Set a value locally (does not affect parent)
  template <typename T, typename U>
  void set(const void* key, U&& value) {
    // Preserve T even when it is std::any or an in-place construction tag.
    data.insert_or_assign(key, std::make_any<T>(T(std::forward<U>(value))));
  }

  // Check if key exists locally or in parent chain
  bool contains(const void* key) const {
    for (auto* storage = this; storage; storage = storage->parent.get()) {
      if (storage->data.count(key) > 0) {
        return true;
      }
    }
    return false;
  }

  // Remove a value locally
  void erase(const void* key) {
    data.erase(key);
  }
};

}  // namespace detail
}  // namespace coro
