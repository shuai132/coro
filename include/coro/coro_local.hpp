#pragma once

#ifndef CORO_ENABLE_LOCAL_STORAGE
#error "coro_local.hpp requires CORO_ENABLE_LOCAL_STORAGE to be defined"
#endif

#include <coroutine>
#include <memory>
#include <optional>
#include <type_traits>

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
// Directly awaited children inherit parent values; set() writes local overrides.
// get_ptr() may borrow an ancestor's value and does not copy it before mutation.
template <typename T>
struct coro_local {
 private:
  // Nonempty keys cannot share an address through empty-base or no_unique_address optimization.
  char identity_{};

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
    std::optional<T> result_{};

    [[nodiscard]] bool await_ready() const noexcept {
      return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      T* ptr = self_->find_in_storage(h.promise());
      if (ptr) {
        result_.emplace(*ptr);
      } else {
        result_.emplace();
      }
      return false;  // Don't actually suspend
    }

    T await_resume() noexcept(std::is_nothrow_move_constructible_v<T>) {
      return std::move(*result_);
    }
  };

  // Awaiter for getting optional value (returns std::optional<T>)
  struct get_optional_awaiter {
    const coro_local* self_;
    std::optional<T> result_{};

    [[nodiscard]] bool await_ready() const noexcept {
      return false;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      T* ptr = self_->find_in_storage(h.promise());
      if (ptr) {
        result_.emplace(*ptr);
      } else {
        result_.reset();
      }
      return false;
    }

    std::optional<T> await_resume() noexcept(std::is_nothrow_move_constructible_v<T>) {
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
    bool await_suspend(std::coroutine_handle<Promise> h) {
      h.promise().ensure_local_storage();
      auto& storage = h.promise().local_storage_;
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
      if (h.promise().owns_local_storage_) {
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

  // Borrow a pointer to the stored value, possibly in an ancestor; no copy is made.
  // Usage: T* ptr = co_await storage.get_ptr();
  [[nodiscard]] auto get_ptr() const noexcept {
    return get_ptr_awaiter{this};
  }

  // Set the value for the current coroutine
  // Usage: co_await storage.set(value);
  template <typename U>
  auto set(U&& value) const noexcept(std::is_nothrow_constructible_v<U, U&&>) {
    return set_awaiter<U>{this, std::forward<U>(value)};
  }

  // Check if a value is stored (locally or in parent chain)
  // Usage: bool exists = co_await storage.has();
  [[nodiscard]] auto has() const noexcept {
    return has_awaiter{this};
  }

  // Erase the local value, making any inherited value visible again
  // Usage: co_await storage.erase();
  auto erase() const noexcept {
    return erase_awaiter{this};
  }
};

namespace detail {

// Inheritance is automatic; explicitly materialize a local override layer when requested.
struct inherit_coro_local_awaiter {
  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  template <typename Promise>
  bool await_suspend(std::coroutine_handle<Promise> h) {
    h.promise().ensure_local_storage();
    return false;
  }

  void await_resume() noexcept {}
};

}  // namespace detail

// Ensure the current coroutine has a local storage map.
// Direct co_await inheritance and local set() overrides already work automatically.
// Usage: co_await inherit_coro_local();
[[nodiscard]] inline detail::inherit_coro_local_awaiter inherit_coro_local() noexcept {
  return detail::inherit_coro_local_awaiter{};
}

// Backward compatibility alias
template <typename T>
using local_storage = coro_local<T>;

[[nodiscard]] inline detail::inherit_coro_local_awaiter inherit_local_storage() noexcept {
  return detail::inherit_coro_local_awaiter{};
}

}  // namespace coro
