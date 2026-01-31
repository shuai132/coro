#pragma once

#ifndef CORO_ENABLE_LOCAL_STORAGE
#error "coro_local.hpp requires CORO_ENABLE_LOCAL_STORAGE to be defined"
#endif

#include <coroutine>
#include <memory>
#include <optional>

#include "coro/coro.hpp"

namespace coro {

// Forward declaration
template <typename T>
struct coro_local;

// Coroutine-local storage similar to thread_local but scoped to coroutines.
// Each coro_local<T> instance acts as a unique key for storing values.
//
// Usage:
//   static coro_local<int> my_value;  // Define a storage key (like thread_local)
//
//   async<void> my_coro() {
//     co_await my_value.set(42);         // Set value for current coroutine
//     int v = co_await my_value.get();   // Get value (returns 42)
//   }
//
// Child coroutines inherit parent's storage values (copy-on-write semantics)
template <typename T>
struct coro_local {
 private:
  // Helper to find value in storage or parent chain
  template <typename Promise>
  T* find_in_storage(Promise& promise) const {
    if (promise.local_storage_) {
      return promise.local_storage_->template find<T>(this);
    }
    return nullptr;
  }

  // Helper to check if value exists in storage or parent chain
  template <typename Promise>
  bool exists_in_storage(Promise& promise) const {
    if (promise.local_storage_) {
      return promise.local_storage_->contains(this);
    }
    return false;
  }

  // Awaiter for getting the stored value
  struct get_awaiter {
    const coro_local* self_;
    T result_{};

    [[nodiscard]] bool await_ready() const noexcept {
      return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
      T* ptr = self_->find_in_storage(h.promise());
      if (ptr) {
        result_ = *ptr;
      }
      return false;  // Don't actually suspend
    }

    T await_resume() noexcept {
      return std::move(result_);
    }
  };

  // Awaiter for getting optional value (returns std::optional<T>)
  struct get_optional_awaiter {
    const coro_local* self_;
    std::optional<T> result_;

    [[nodiscard]] bool await_ready() const noexcept {
      return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
      T* ptr = self_->find_in_storage(h.promise());
      if (ptr) {
        result_ = *ptr;
      }
      return false;
    }

    std::optional<T> await_resume() noexcept {
      return std::move(result_);
    }
  };

  // Awaiter for getting pointer to stored value (nullptr if not found)
  struct get_ptr_awaiter {
    const coro_local* self_;
    T* result_ = nullptr;

    [[nodiscard]] bool await_ready() const noexcept {
      return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
      result_ = self_->find_in_storage(h.promise());
      return false;
    }

    T* await_resume() noexcept {
      return result_;
    }
  };

  // Awaiter for setting a value
  template <typename U>
  struct set_awaiter {
    const coro_local* self_;
    U value_;

    [[nodiscard]] bool await_ready() const noexcept {
      return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
      auto& storage = h.promise().local_storage_;
      if (!storage) {
        storage = std::make_shared<detail::coro_local_map>();
      }
      storage->template set<T>(self_, std::forward<U>(value_));
      return false;
    }

    void await_resume() noexcept {}
  };

  // Awaiter for checking if value exists
  struct has_awaiter {
    const coro_local* self_;
    bool result_ = false;

    [[nodiscard]] bool await_ready() const noexcept {
      return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
      result_ = self_->exists_in_storage(h.promise());
      return false;
    }

    bool await_resume() noexcept {
      return result_;
    }
  };

  // Awaiter for erasing a value
  struct erase_awaiter {
    const coro_local* self_;

    [[nodiscard]] bool await_ready() const noexcept {
      return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
      auto& storage = h.promise().local_storage_;
      if (storage) {
        storage->erase(self_);
      }
      return false;
    }

    void await_resume() noexcept {}
  };

 public:
  coro_local() = default;

  // Disable copy and move to ensure each instance has a unique address
  coro_local(const coro_local&) = delete;
  coro_local(coro_local&&) = delete;
  coro_local& operator=(const coro_local&) = delete;
  coro_local& operator=(coro_local&&) = delete;

  // Get the stored value, returns default-constructed T if not found
  // Usage: T value = co_await storage.get();
  [[nodiscard]] auto get() const noexcept {
    return get_awaiter{this};
  }

  // Get the stored value as optional, returns std::nullopt if not found
  // Usage: std::optional<T> value = co_await storage.get_optional();
  [[nodiscard]] auto get_optional() const noexcept {
    return get_optional_awaiter{this};
  }

  // Get pointer to stored value, returns nullptr if not found
  // Usage: T* ptr = co_await storage.get_ptr();
  [[nodiscard]] auto get_ptr() const noexcept {
    return get_ptr_awaiter{this};
  }

  // Set the value for the current coroutine
  // Usage: co_await storage.set(value);
  template <typename U>
  auto set(U&& value) const noexcept {
    return set_awaiter<U>{this, std::forward<U>(value)};
  }

  // Check if a value is stored (locally or in parent chain)
  // Usage: bool exists = co_await storage.has();
  [[nodiscard]] auto has() const noexcept {
    return has_awaiter{this};
  }

  // Erase the locally stored value
  // Usage: co_await storage.erase();
  auto erase() const noexcept {
    return erase_awaiter{this};
  }
};

namespace detail {

// Awaiter for inheriting parent coroutine's coro_local storage
// Note: With the simplified single-pointer design, inheritance is automatic
// when parent has local_storage_. This function now just ensures local storage exists.
struct inherit_coro_local_awaiter {
  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  template <typename Promise>
  bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
    // With single-pointer design, parent link is already set up in await_suspend
    // This just ensures we have our own local storage for copy-on-write
    auto& storage = h.promise().local_storage_;
    if (!storage) {
      storage = std::make_shared<coro_local_map>();
    }
    return false;
  }

  void await_resume() noexcept {}
};

}  // namespace detail

// Explicitly inherit coro_local storage from parent coroutine
// This is useful when you want copy-on-write semantics where
// the child sees parent values but modifications are local
// Usage: co_await inherit_coro_local();
[[nodiscard]] inline detail::inherit_coro_local_awaiter inherit_coro_local() {
  return detail::inherit_coro_local_awaiter{};
}

// Backward compatibility alias
template <typename T>
using local_storage = coro_local<T>;

[[nodiscard]] inline detail::inherit_coro_local_awaiter inherit_local_storage() {
  return detail::inherit_coro_local_awaiter{};
}

}  // namespace coro
