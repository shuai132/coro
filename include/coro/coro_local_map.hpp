#pragma once

#include <any>
#include <memory>
#include <unordered_map>

namespace coro {
namespace detail {

// Type-erased storage container for coroutine-local data
// Each coroutine can have its own storage map, and child coroutines
// can optionally inherit from parent coroutines
struct coro_local_map {
  std::unordered_map<const void*, std::any> data;
  std::shared_ptr<coro_local_map> parent;  // Parent storage for inheritance

  // Look up a value by key, checking parent chain if not found locally
  template <typename T>
  T* find(const void* key) {
    auto it = data.find(key);
    if (it != data.end()) {
      return std::any_cast<T>(&it->second);
    }
    if (parent) {
      return parent->find<T>(key);
    }
    return nullptr;
  }

  // Set a value locally (does not affect parent)
  template <typename T, typename U>
  void set(const void* key, U&& value) {
    data[key] = T(std::forward<U>(value));
  }

  // Check if key exists locally or in parent chain
  bool contains(const void* key) const {
    if (data.count(key) > 0) {
      return true;
    }
    if (parent) {
      return parent->contains(key);
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
