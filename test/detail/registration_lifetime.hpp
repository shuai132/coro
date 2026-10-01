#pragma once

#include <functional>
#include <memory>
#include <mutex>

#include "assert_def.h"
#include "coro/coro.hpp"

inline void check_coro_leak();

struct registration_lifetime_state {
  std::unique_ptr<coro::async<void>> owner;
  std::function<void()> release;
  int trigger_unlock = 2;
  bool armed = false;
  bool mutex_destroyed = false;
  bool completed = false;
};

inline registration_lifetime_state* current_registration_lifetime_state;

struct registration_lifetime_mutex {
  std::mutex mutex_;
  registration_lifetime_state* state_ = current_registration_lifetime_state;
  int unlock_count_ = 0;

  ~registration_lifetime_mutex() {
    state_->mutex_destroyed = true;
  }
  void lock() {
    mutex_.lock();
  }
  void unlock() {
    auto* state = state_;
    mutex_.unlock();
    if (state->armed && ++unlock_count_ == state->trigger_unlock) {
      state->owner.reset();
      state->release();
      ASSERT(!state->mutex_destroyed);
    }
  }
};

template <typename FACTORY>
void check_registration_lifetime(FACTORY factory) {
  registration_lifetime_state state;
  current_registration_lifetime_state = &state;
  state.owner = std::make_unique<coro::async<void>>(factory(state));
  state.owner->current_coro_handle_.resume();
  if (!state.completed) {
    state.release();
  }
  ASSERT(state.completed);
  ASSERT(state.mutex_destroyed);
  ASSERT(!state.owner);
  current_registration_lifetime_state = nullptr;
  check_coro_leak();
}
