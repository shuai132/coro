/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include <array>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "log.h"
#include "noinline.hpp"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include "assert_def.h"
#include "coro.hpp"
#include "exception_mutex.hpp"
#include "registration_lifetime.hpp"
#include "utils.hpp"

using namespace coro;

template <typename T>
struct is_standard_channel_owner : std::false_type {};

template <typename T>
struct is_standard_channel_owner<std::shared_ptr<T>> : std::true_type {};

void test_channel_owner_selection() {
  // Keep the optimization limited to the exact dummy_mutex specialization.
  static_assert(!is_standard_channel_owner<decltype(std::declval<channel_st<int>>().recv().state_)>::value);
  static_assert(is_standard_channel_owner<decltype(std::declval<channel_mt<int>>().recv().state_)>::value);
  static_assert(is_standard_channel_owner<decltype(std::declval<channel<int, registration_lifetime_mutex>>().recv().state_)>::value);
  static_assert(std::is_same_v<channel_st<int>, channel<int, dummy_mutex>>);
  static_assert(std::is_same_v<decltype(std::declval<channel_st<int>>().recv().state_->closed_), detail::atomic_st<bool>>);
  static_assert(std::is_same_v<decltype(std::declval<channel_mt<int>>().recv().state_->closed_), std::atomic<bool>>);
  static_assert(std::is_same_v<decltype(std::declval<channel<int, registration_lifetime_mutex>>().recv().state_->closed_), std::atomic<bool>>);
}

struct channel_lifetime_value {
  int& live_;

  explicit channel_lifetime_value(int& live) : live_(live) {
    ++live_;
  }
  ~channel_lifetime_value() {
    --live_;
  }
};

void test_shared_ptr_st_lifetime() {
  using owner = detail::shared_ptr_st<channel_lifetime_value>;
  static_assert(std::is_nothrow_copy_constructible_v<owner>);
  static_assert(std::is_nothrow_move_constructible_v<owner>);
  static_assert(std::is_nothrow_copy_assignable_v<owner>);
  static_assert(std::is_nothrow_move_assignable_v<owner>);
  int live = 0;
  int replaced_live = 0;
  {
    auto first = owner::make(live);
    auto copy = first;
    auto moved = std::move(copy);
    auto* alias = &moved;
    moved = *alias;
    moved = std::move(*alias);
    auto replacement = owner::make(replaced_live);
    replacement = first;
    ASSERT(replaced_live == 0);
    first = {};
    replacement = std::move(moved);
    ASSERT(live == 1);
    ASSERT(replacement->live_ == 1);
  }
  ASSERT(live == 0);
}

void test_channel_unstarted_awaiter_lifetime(bool drain) {
  using channel_type = channel_st<std::shared_ptr<channel_lifetime_value>>;
  int live = 0;
  std::optional<channel_type::recv_awaitable> last_receiver;
  {
    auto ch = std::make_unique<channel_type>(1);
    auto frame = []() -> async<void> {
      co_return;
    }();
    {
      auto seed = ch->send(std::make_shared<channel_lifetime_value>(live));
      ASSERT(!seed.await_ready());
      ASSERT(!seed.await_suspend(frame.current_coro_handle_));
      ASSERT(seed.await_resume());
    }
    last_receiver.emplace(ch->recv());
    auto sending = ch->send(nullptr);
    auto broadcasting = ch->broadcast(nullptr);
    ch.reset();
    ASSERT(live == 1);
    ASSERT(sending.await_ready());
    ASSERT(!sending.await_resume());
    ASSERT(broadcasting.await_ready());
    ASSERT(broadcasting.await_resume() == 0);
  }
  ASSERT(live == 1);
  if (drain) {
    auto frame = []() -> async<void> {
      co_return;
    }();
    ASSERT(!last_receiver->await_ready());
    ASSERT(!last_receiver->await_suspend(frame.current_coro_handle_));
    auto value = last_receiver->await_resume();
    ASSERT(value && *value);
    ASSERT(live == 1);
  }
  last_receiver.reset();
  ASSERT(live == 0);
  check_coro_leak();
}

async<void> channel_destroyed_waiter(channel_st<std::unique_ptr<channel_lifetime_value>>* ch, bool sending, int& live, int& completed) {
  if (sending) {
    ASSERT(!(co_await ch->send(std::make_unique<channel_lifetime_value>(live))));
  } else {
    ASSERT(!(co_await ch->recv()));
  }
  ++completed;
}

void test_channel_pending_awaiter_lifetime(bool sending) {
  executor_poll exec;
  int live = 0;
  int completed = 0;
  auto ch = std::make_unique<channel_st<std::unique_ptr<channel_lifetime_value>>>();
  for (int i = 0; i < 3; ++i) {
    channel_destroyed_waiter(ch.get(), sending, live, completed).detach(exec);
  }
  for (int i = 0; i < 3; ++i) {
    exec.poll_one();
  }
  ASSERT(completed == 0);
  ASSERT(live == (sending ? 3 : 0));
  ch.reset();
  ASSERT(completed == 0);
  for (int i = 0; i < 3; ++i) {
    exec.poll_one();
  }
  ASSERT(completed == 3);
  ASSERT(live == 0);
  check_coro_leak();
}

void test_channel_lock_failure() {
  static_assert(!noexcept(std::declval<channel_mt<int>&>().recv().await_ready()));
  static_assert(noexcept(std::declval<channel_st<int>&>().recv().await_ready()));
  static_assert(noexcept(std::declval<const channel_st<int>&>().empty()));
  static_assert(noexcept(std::declval<const channel_st<int>&>().full()));
  static_assert(noexcept(std::declval<const channel_st<int>&>().size()));
#ifndef CORO_DISABLE_EXCEPTION
  channel<int, exception_mutex> channel;
  auto receiving = channel.recv();
  auto frame = []() -> async<void> {
    co_return;
  }();
  ASSERT(!receiving.await_ready());
  expect_lock_failure([&] {
    (void)receiving.await_suspend(frame.current_coro_handle_);
  });
  expect_lock_failure([&] {
    (void)channel.empty();
  });
  expect_lock_failure([&] {
    (void)channel.full();
  });
  expect_lock_failure([&] {
    (void)channel.size();
  });
  ASSERT(!receiving.await_ready());
  channel.close();
  expect_lock_failure([&] {
    (void)receiving.await_ready();
  });
  ASSERT(receiving.await_ready());
  ASSERT(!receiving.await_resume());
#endif
}

async<void> channel_registration_handle() {
  co_return;
}

struct counted_channel_mutex {
  inline static int locks = 0;
  std::mutex mutex_;

  void lock() {
    ++locks;
    mutex_.lock();
  }
  void unlock() noexcept {
    mutex_.unlock();
  }
};

void test_receive_lock_count_and_close() {
  channel<int, counted_channel_mutex> ch(1);
  auto frame = channel_registration_handle();
  auto sending = ch.send(42);
  ASSERT(!sending.await_suspend(frame.current_coro_handle_));
  ASSERT(sending.await_resume());
  auto receiving = ch.recv();
  counted_channel_mutex::locks = 0;
  ASSERT(!receiving.await_ready());
  ASSERT(counted_channel_mutex::locks == 0);
  ASSERT(!receiving.await_suspend(frame.current_coro_handle_));
  ASSERT(counted_channel_mutex::locks == 1);
  ASSERT(receiving.await_resume() == 42);

  // Closing between the readiness check and registration must still complete immediately.
  ASSERT(!receiving.await_ready());
  ch.close();
  ASSERT(!receiving.await_suspend(frame.current_coro_handle_));
  ASSERT(!receiving.await_resume());
  counted_channel_mutex::locks = 0;
  ASSERT(receiving.await_ready());
  ASSERT(counted_channel_mutex::locks == 1);
  ASSERT(!receiving.await_resume());
}

struct channel_move_count {
  int moves = 0;
  int copies = 0;
  int live = 0;
};

struct counted_channel_value {
  channel_move_count* counts_;

  explicit counted_channel_value(channel_move_count& counts) : counts_(&counts) {
    ++counts_->live;
  }
  counted_channel_value(const counted_channel_value& other) : counts_(other.counts_) {
    ++counts_->copies;
    ++counts_->live;
  }
  counted_channel_value(counted_channel_value&& other) noexcept : counts_(other.counts_) {
    ++counts_->moves;
    ++counts_->live;
  }
  ~counted_channel_value() {
    --counts_->live;
  }
};

template <typename CHANNEL>
void test_channel_awaiter_value_moves() {
  CHANNEL ch;
  channel_move_count counts;
  {
    auto sending = ch.send(counted_channel_value{counts});
    ASSERT(counts.moves == 1 && counts.copies == 0 && counts.live == 1);
    // Direct construction from an lvalue remains supported.
    typename CHANNEL::send_awaitable copy{sending.state_, sending.value_};
    ASSERT(counts.copies == 1 && counts.live == 2);
  }
  ASSERT(counts.live == 0);
  counts = {};
  {
    auto broadcasting = ch.broadcast(counted_channel_value{counts});
    ASSERT(counts.moves == 1 && counts.copies == 0 && counts.live == 1);
    typename CHANNEL::broadcast_awaitable copy{broadcasting.state_, broadcasting.value_};
    ASSERT(counts.copies == 1 && counts.live == 2);
  }
  ASSERT(counts.live == 0);
}

async<void> channel_receiver_registration_lifetime(registration_lifetime_state& state) {
  channel<int, registration_lifetime_mutex> channel;
  auto handle_owner = channel_registration_handle();
  state.release = [&] {
    auto sender = channel.send(42);
    if (!sender.await_ready()) {
      ASSERT(!sender.await_suspend(handle_owner.current_coro_handle_));
    }
    ASSERT(sender.await_resume());
  };
  state.trigger_unlock = 1;
  state.armed = true;
  auto result = co_await channel.recv();
  ASSERT(result && *result == 42);
  state.completed = true;
}

async<void> channel_sender_registration_lifetime(registration_lifetime_state& state) {
  channel<int, registration_lifetime_mutex> channel;
  auto handle_owner = channel_registration_handle();
  state.release = [&] {
    auto receiver = channel.recv();
    if (!receiver.await_ready()) {
      ASSERT(!receiver.await_suspend(handle_owner.current_coro_handle_));
    }
    ASSERT(receiver.await_resume().value() == 42);
  };
  // send.await_ready does not lock, so wait for its first queue unlock rather than the second.
  state.trigger_unlock = 1;
  auto sender = channel.send(42);
  state.armed = true;
  ASSERT(co_await sender);
  state.completed = true;
}

struct channel_chain_executor : executor_poll {
  int depth = 0;
  int max_depth = 0;

  void dispatch(std::function<void()> task) override {
    ++depth;
    if (depth > max_depth) {
      max_depth = depth;
    }
    executor_poll::dispatch(std::move(task));
    --depth;
  }
};

enum class channel_chain_mode { forward, reverse, buffered, close_receivers, close_senders };

template <typename CHANNEL>
async<void> channel_chain_stage(CHANNEL& input, CHANNEL& output, channel_chain_mode mode) {
  if (mode == channel_chain_mode::close_receivers) {
    ASSERT(!(co_await input.recv()));
    output.close();
  } else if (mode == channel_chain_mode::close_senders) {
    ASSERT(!(co_await output.send(42)));
    input.close();
  } else if (mode == channel_chain_mode::forward) {
    auto value = co_await input.recv();
    ASSERT(value && *value == 42);
    ASSERT(co_await output.send(*value));
  } else {
    ASSERT(co_await output.send(42));
    auto value = co_await input.recv();
    ASSERT(value && *value == 42);
  }
}

template <typename CHANNEL>
async<void> channel_chain_endpoint(CHANNEL& ch, bool sending, bool closing) {
  if (sending) {
    ASSERT((co_await ch.send(42)) == !closing);
  } else {
    auto value = co_await ch.recv();
    if (closing) {
      ASSERT(!value);
    } else {
      ASSERT(value && *value == 42);
    }
  }
}

template <typename CHANNEL>
async<void> fill_channel_chain(std::vector<std::unique_ptr<CHANNEL>>& channels) {
  for (auto& ch : channels) {
    ASSERT(co_await ch->send(42));
  }
}

template <typename CHANNEL>
void test_channel_wakeup_chain() {
  constexpr int stages = 128;
  for (auto mode : {channel_chain_mode::forward, channel_chain_mode::reverse, channel_chain_mode::buffered, channel_chain_mode::close_receivers,
                    channel_chain_mode::close_senders}) {
    channel_chain_executor executor;
    std::vector<std::unique_ptr<CHANNEL>> channels;
    for (int index = 0; index <= stages; ++index) {
      channels.push_back(std::make_unique<CHANNEL>(mode == channel_chain_mode::buffered ? 1 : 0));
    }
    if (mode == channel_chain_mode::buffered) {
      bool filled = false;
      fill_channel_chain(channels).detach(executor, [&] {
        filled = true;
      });
      executor.poll_one();
      ASSERT(filled);
    }
    int completed = 0;
    auto on_completed = [&] {
      ++completed;
    };
    for (int index = 0; index < stages; ++index) {
      channel_chain_stage(*channels[index], *channels[index + 1], mode).detach(executor, on_completed);
    }
    const bool forward = mode == channel_chain_mode::forward || mode == channel_chain_mode::close_receivers;
    const bool closing = mode == channel_chain_mode::close_receivers || mode == channel_chain_mode::close_senders;
    channel_chain_endpoint(forward ? *channels.back() : *channels.front(), !forward, closing).detach(executor, on_completed);
    // Every stage must reach its first wait before the transfer or close begins.
    for (int index = 0; index <= stages; ++index) {
      executor.poll_one();
    }
    ASSERT(completed == 0);
    int expected = stages + 1;
    if (closing) {
      executor.post([&] {
        (forward ? channels.front() : channels.back())->close();
      });
    } else {
      ++expected;
      channel_chain_endpoint(forward ? *channels.front() : *channels.back(), forward, false).detach(executor, on_completed);
    }
    for (int attempt = 0; completed != expected && attempt < 10000; ++attempt) {
      executor.poll_one();
    }
    ASSERT(completed == expected);
    ASSERT(executor.max_depth <= 8);
    if (mode == channel_chain_mode::buffered) {
      for (auto& ch : channels) {
        ASSERT(ch->size() == 1);
      }
    }
    channels.clear();
    check_coro_leak();
  }
}

template <typename CHANNEL>
async<void> stopped_loop_channel_sender(CHANNEL& ch, executor_loop& executor, bool stop_before_send) {
  co_await sleep(1ms);
  if (stop_before_send) {
    executor.stop();
  }
  ASSERT(co_await ch.send(42));
}

template <typename CHANNEL>
void test_stopped_loop_channel_resumption(bool stop_before_send) {
  std::weak_ptr<int> retained;
  {
    executor_loop executor;
    CHANNEL ch;
    bool resumed = false;
    bool sender_completed = false;
    auto token = std::make_shared<int>(42);
    retained = token;
    // Keep the frame unstarted and owned so a rejected wakeup can be released safely.
    auto make_receiver = [](bool& called, std::shared_ptr<int> resource) -> async<void> {
      ASSERT(*resource == 42);
      called = true;
      co_return;
    };
    auto receiver = make_receiver(resumed, std::move(token)).bind_executor(executor);
    auto receiving = ch.recv();
    ASSERT(!receiving.await_ready());
    ASSERT(receiving.await_suspend(receiver.current_coro_handle_));
    executor.post([&] {
      stopped_loop_channel_sender(ch, executor, stop_before_send).detach(executor, [&] {
        ASSERT(!resumed);  // Waking a receiver must queue it rather than dispatch inline.
        sender_completed = true;
        if (!stop_before_send) {
          // Let the accepted receiver wakeup run before stopping.
          executor.post([&] {
            executor.stop();
          });
        }
      });
    });
    executor.run_loop();
    ASSERT(sender_completed);
    ASSERT(resumed == !stop_before_send);
    ASSERT(receiving.await_resume() == 42);
  }
  ASSERT(retained.expired());
  check_coro_leak();
}

thread_local size_t* channel_allocation_bytes = nullptr;

CORO_TEST_NOINLINE void* operator new(std::size_t size) {
  if (auto* memory = std::malloc(size ? size : 1)) {
    if (channel_allocation_bytes) {
      *channel_allocation_bytes += size;
    }
    return memory;
  }
#ifndef CORO_DISABLE_EXCEPTION
  throw std::bad_alloc();
#else
  std::abort();
#endif
}

CORO_TEST_NOINLINE void operator delete(void* memory) noexcept {
  std::free(memory);
}

CORO_TEST_NOINLINE void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}

template <typename CHANNEL>
CORO_TEST_NOINLINE void construct_channel_allocation_probe(std::optional<CHANNEL>& ch) {
  // Isolate construction from the allocation counter: GCC 14 otherwise reports a
  // spurious uninitialized reference count when optimizing the large-value probe.
  ch.emplace();
}

template <typename CHANNEL>
size_t empty_channel_allocation_bytes() {
  size_t bytes = 0;
  channel_allocation_bytes = &bytes;
  struct allocation_watch_cleanup {
    ~allocation_watch_cleanup() {
      channel_allocation_bytes = nullptr;
    }
  } cleanup;
  {
    std::optional<CHANNEL> ch;
    construct_channel_allocation_probe(ch);
    ASSERT(ch->empty());
    ASSERT(!ch->full());
    ASSERT(ch->size() == 0);
  }
  return bytes;
}

template <template <typename> typename CHANNEL>
void test_unbuffered_channel_does_not_allocate_message_storage() {
  auto small_bytes = empty_channel_allocation_bytes<CHANNEL<int>>();
  auto large_bytes = empty_channel_allocation_bytes<CHANNEL<std::array<char, 8192>>>();
  // An unbuffered channel should allocate metadata, not storage for queued message values.
  ASSERT(small_bytes == large_bytes);
}

template <typename CHANNEL>
async<void> reused_queued_sender(CHANNEL& ch, event& completed) {
  auto sending = ch.send(42);
  ASSERT(co_await sending);
  ASSERT(!(co_await sending));
  completed.set();
}

template <typename CHANNEL>
async<void> test_reused_send_results() {
  {
    CHANNEL ch(1);
    auto sending = ch.send(42);
    ASSERT(co_await sending);
    ch.close();
    ASSERT(!(co_await sending));
    ASSERT(!(co_await sending));
    auto value = co_await ch.recv();
    ASSERT(value && *value == 42);
    ASSERT(!(co_await ch.recv()));
  }
  {
    CHANNEL ch(1);
    event completed;
    co_await spawn_local(reused_queued_sender(ch, completed));
    ASSERT(!completed.is_set());
    ch.close();
    co_await completed;
    ASSERT(completed.is_set());
  }
}

template <typename CHANNEL>
async<void> test_reused_receive_results() {
  CHANNEL ch(2);
  ASSERT(co_await ch.send(1));
  ASSERT(co_await ch.send(2));
  auto receiving = ch.recv();
  ASSERT((co_await receiving) == 1);
  ch.close();
  ASSERT((co_await receiving) == 2);
  ASSERT(!(co_await receiving));
  ASSERT(!(co_await receiving));
}

uint64_t get_now_ms() {
  auto now = std::chrono::steady_clock::now();
  auto epoch = now.time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
}

async<void> reservation_handle_owner() {
  co_return;
}

struct receive_reentrancy_state {
  std::function<void()> on_move;
  std::function<void()> on_destroy;
  int live = 0;
};

struct reentrant_channel_value {
  receive_reentrancy_state* state;

  explicit reentrant_channel_value(receive_reentrancy_state& value_state) : state(&value_state) {
    ++state->live;
  }
  reentrant_channel_value(const reentrant_channel_value&) = delete;
  reentrant_channel_value(reentrant_channel_value&& other) : state(other.state) {
    ++state->live;
    if (state->on_move) {
      state->on_move();
    }
  }
  ~reentrant_channel_value() {
    --state->live;
    if (state->on_destroy) {
      state->on_destroy();
    }
  }
};

template <typename CHANNEL>
void test_receive_result_reentrancy() {
  for (bool on_move : {false, true}) {
    receive_reentrancy_state state;
    {
      CHANNEL ch(1);
      auto owner = reservation_handle_owner();
      auto sending = ch.send(reentrant_channel_value{state});
      ASSERT(!sending.await_ready());
      ASSERT(!sending.await_suspend(owner.current_coro_handle_));
      ASSERT(sending.await_resume());
      auto receiving = ch.recv();
      ASSERT(!receiving.await_ready());
      ASSERT(!receiving.await_suspend(owner.current_coro_handle_));

      // The receive now owns its value; extracting it may inspect or close the channel.
      int calls = 0;
      auto& hook = on_move ? state.on_move : state.on_destroy;
      hook = [&] {
        ASSERT(ch.empty());
        ch.close();
        ++calls;
      };
      auto consume = [&] {
        auto result = receiving.await_resume();
        ASSERT(result && result->state == &state);
      };
      if constexpr (std::is_same_v<CHANNEL, channel_st<reentrant_channel_value>>) {
        consume();
      } else {
        std::atomic<bool> completed{false};
        std::thread worker([&] {
          consume();
          completed.store(true, std::memory_order_release);
        });
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!completed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(1ms);
        }
        ASSERT(completed.load(std::memory_order_acquire));
        worker.join();
      }
      ASSERT(calls > 0);
      ASSERT(ch.is_closed());
      ASSERT(!receiving.pending_value_);
      hook = {};
    }
    ASSERT(state.live == 0);
    check_coro_leak();
  }
}

template <typename CHANNEL>
async<void> reservation_receiver(CHANNEL& ch, int& result, event& completed) {
  auto value = co_await ch.recv();
  ASSERT(value);
  result = *value;
  completed.set();
}

template <typename CHANNEL>
async<void> test_buffered_receive_reservation() {
  CHANNEL ch(1);
  co_await ch.send(1);
  auto first = ch.recv();
  auto handle_owner = reservation_handle_owner();
  ASSERT(!first.await_ready());
  ASSERT(!first.await_suspend(handle_owner.current_coro_handle_));

  // Interleave another receiver between the first await_suspend and await_resume.
  // A ready receive must already own its value when it releases the channel lock.
  event second_completed;
  int second_value = 0;
  co_await spawn_local(reservation_receiver(ch, second_value, second_completed));
  auto first_value = first.await_resume();
  ASSERT(first_value && *first_value == 1);
  ASSERT(!second_completed.is_set());
  co_await ch.send(2);
  co_await second_completed;
  ASSERT(second_completed.is_set());
  ASSERT(second_value == 2);
  ASSERT(ch.empty());
}

#ifndef CORO_DISABLE_EXCEPTION
struct transfer_state {
  int fail_value = -1;
  int live = 0;
};

struct throwing_channel_value {
  transfer_state* state;
  int value;

  throwing_channel_value(transfer_state& transfer, int v) : state(&transfer), value(v) {
    ++state->live;
  }

  throwing_channel_value(const throwing_channel_value&) = delete;
  throwing_channel_value(throwing_channel_value&& other) : state(other.state), value(other.value) {
    if (state->fail_value == value) {
      throw std::runtime_error("channel transfer failed");
    }
    ++state->live;
  }

  ~throwing_channel_value() {
    --state->live;
  }
};

template <typename CHANNEL>
async<void> transfer_receiver(CHANNEL& ch, event& completed) {
  auto value = co_await ch.recv();
  ASSERT(!value);
  completed.set();
}

template <typename CHANNEL>
async<void> transfer_sender(CHANNEL& ch, transfer_state& state, int value, event& completed) {
  bool delivered = co_await ch.send(throwing_channel_value{state, value});
  ASSERT(!delivered);
  completed.set();
}

template <typename CHANNEL>
async<void> test_failed_send_keeps_receiver() {
  transfer_state state;
  CHANNEL ch;
  event receiver_completed;
  co_await spawn_local(transfer_receiver(ch, receiver_completed));
  {
    auto sending = ch.send(throwing_channel_value{state, 42});
    state.fail_value = 42;
    bool caught = false;
    try {
      co_await sending;
    } catch (const std::runtime_error&) {
      caught = true;
    }
    state.fail_value = -1;
    ASSERT(caught);
    ch.close();
    co_await receiver_completed;
    ASSERT(receiver_completed.is_set());
  }
  ASSERT(state.live == 0);
}

template <typename CHANNEL>
async<void> test_failed_receive_keeps_sender(bool buffered) {
  transfer_state state;
  CHANNEL ch(buffered ? 1 : 0);
  if (buffered) {
    co_await ch.send(throwing_channel_value{state, 1});
  }
  event sender_completed;
  co_await spawn_local(transfer_sender(ch, state, 42, sender_completed));
  state.fail_value = 42;
  bool caught = false;
  try {
    co_await ch.recv();
  } catch (const std::runtime_error&) {
    caught = true;
  }
  state.fail_value = -1;
  ASSERT(caught);
  ch.close();
  co_await sender_completed;
  ASSERT(sender_completed.is_set());
  ASSERT(state.live == 0);
}

template <typename CHANNEL>
async<void> test_failed_receive_extraction(bool queued_close) {
  transfer_state state;
  CHANNEL ch(1);
  co_await ch.send(throwing_channel_value{state, 42});
  auto receiving = ch.recv();
  auto owner = reservation_handle_owner();
  ASSERT(!receiving.await_ready());
  ASSERT(!receiving.await_suspend(owner.current_coro_handle_));

  // Fail after delivery has reserved the value, while extracting it for the caller.
  state.fail_value = 42;
  bool caught = false;
  try {
    receiving.await_resume();
  } catch (const std::runtime_error&) {
    caught = true;
  }
  ASSERT(caught);
  ASSERT(state.live == 0);
  state.fail_value = -1;
  if (queued_close) {
    auto* exec = co_await current_executor();
    exec->post([&ch] {
      ch.close();
    });
  } else {
    ch.close();
  }
  ASSERT(!(co_await receiving));
  ASSERT(state.live == 0);
  ASSERT(!(co_await receiving));
}

template <typename CHANNEL>
async<void> test_reused_receive_after_refill_failure(bool close_before_retry) {
  transfer_state state;
  CHANNEL ch(1);
  co_await ch.send(throwing_channel_value{state, 1});
  event sender_completed;
  auto sender = [](CHANNEL& ch, transfer_state& state, event& completed, bool expected) -> async<void> {
    ASSERT((co_await ch.send(throwing_channel_value{state, 42})) == expected);
    completed.set();
  };
  co_await spawn_local(sender(ch, state, sender_completed, !close_before_retry));
  ASSERT(!sender_completed.is_set());
  auto receiving = ch.recv();
  state.fail_value = 42;
  bool caught = false;
  try {
    co_await receiving;
  } catch (const std::runtime_error&) {
    caught = true;
  }
  ASSERT(caught);
  ASSERT(!sender_completed.is_set());
  state.fail_value = -1;
  if (close_before_retry) {
    ch.close();
    ASSERT(!(co_await receiving));
  } else {
    auto value = co_await receiving;
    ASSERT(value && value->value == 42);
  }
  co_await sender_completed;
  ASSERT(sender_completed.is_set());
  ASSERT(state.live == 0);
  ch.close();
  ASSERT(!(co_await receiving));
}

template <typename CHANNEL>
async<void> test_failed_refill_preserves_sender_order(size_t capacity, bool close_before_receive) {
  transfer_state state;
  CHANNEL ch(capacity);
  for (size_t i = 0; i < capacity; ++i) {
    ASSERT(co_await ch.send(throwing_channel_value{state, static_cast<int>(i)}));
  }
  auto sender = [](CHANNEL& ch, transfer_state& state, int value, bool expected, event& completed) -> async<void> {
    ASSERT((co_await ch.send(throwing_channel_value{state, value})) == expected);
    completed.set();
  };
  event first_completed;
  event second_completed;
  co_await spawn_local(sender(ch, state, 42, !close_before_receive, first_completed));
  ASSERT(!first_completed.is_set());
  state.fail_value = 42;
  bool caught = false;
  try {
    co_await ch.recv();
  } catch (const std::runtime_error&) {
    caught = true;
  }
  ASSERT(caught);
  state.fail_value = -1;
  ASSERT(ch.size() == capacity - 1);

  // A failed refill leaves a free slot, but a new sender must not bypass an older waiter.
  co_await spawn_local(sender(ch, state, 43, !close_before_receive, second_completed));
  ASSERT(!first_completed.is_set());
  ASSERT(!second_completed.is_set());
  ASSERT(ch.size() == capacity - 1);
  if (close_before_receive) {
    ch.close();
  }
  for (size_t i = 1; i < capacity; ++i) {
    auto value = co_await ch.recv();
    ASSERT(value && value->value == static_cast<int>(i));
  }
  if (!close_before_receive) {
    for (int expected : {42, 43}) {
      auto value = co_await ch.recv();
      ASSERT(value && value->value == expected);
    }
    ch.close();
  }
  co_await first_completed;
  ASSERT(first_completed.is_set());
  co_await second_completed;
  ASSERT(second_completed.is_set());
  ASSERT(!(co_await ch.recv()));
  ASSERT(state.live == 0);
}
#endif

// Test basic unbuffered channel
struct channel_completion_state {
  bool consumer_completed = false;
  bool producer_completed = false;
  bool producer_blocked = false;
  uint64_t producer_block_start = 0;
  uint64_t producer_unblock_time = 0;
};

async<void> test_unbuffered_channel() {
  LOG("Starting unbuffered channel test");

  channel<int> ch;  // unbuffered channel

  channel_completion_state state;

  // Producer coroutine that tracks completion and blocking
  auto tracked_producer = [](channel<int>& ch, channel_completion_state& state) -> async<void> {
    LOG("Producer: sending 42 (will block until consumer ready)");
    state.producer_block_start = get_now_ms();
    state.producer_blocked = true;
    co_await ch.send(42);
    state.producer_unblock_time = get_now_ms();
    LOG("Producer: sent 42 (was blocked for %d ms)", int(state.producer_unblock_time - state.producer_block_start));

    LOG("Producer: sending 100 (will block until consumer ready)");
    co_await ch.send(100);
    LOG("Producer: sent 100");

    state.producer_completed = true;
    co_return;
  };

  // Consumer coroutine that tracks completion
  auto tracked_consumer = [](channel<int>& ch, channel_completion_state& state) -> async<void> {
    // Delay to ensure producer starts first and gets blocked
    co_await sleep(50ms);

    // Verify producer is blocked before we start receiving
    ASSERT(state.producer_blocked);
    ASSERT(!state.producer_completed);

    auto val_opt = co_await ch.recv();
    ASSERT(val_opt.has_value());
    int val = *val_opt;
    LOG("Consumer: received %d", val);
    ASSERT(val == 42);

    val_opt = co_await ch.recv();
    ASSERT(val_opt.has_value());
    val = *val_opt;
    LOG("Consumer: received %d", val);
    ASSERT(val == 100);

    LOG("Consumer: received all expected values");
    state.consumer_completed = true;
    co_return;
  };

  // Await both tasks even if the event loop stalls beyond the consumer's timer.
  co_await when_all(tracked_producer(ch, state), tracked_consumer(ch, state));

  // Verify timing: producer should have been unblocked by consumer
  ASSERT(state.producer_unblock_time - state.producer_block_start);
  LOG("Timing verified: producer was unblocked(%d ms) after consumer started receiving",
      int(state.producer_unblock_time - state.producer_block_start));

  // Verify that both have completed
  ASSERT(state.consumer_completed);
  ASSERT(state.producer_completed);
  LOG("Unbuffered channel test completed");
}

// Test basic buffered channel
async<void> test_buffered_channel() {
  LOG("Starting buffered channel test");

  channel<int> ch(2);  // buffered channel with capacity 2

  channel_completion_state state;

  // Producer coroutine that tracks completion and blocking
  auto tracked_producer = [](channel<int>& ch, channel_completion_state& state) -> async<void> {
    LOG("Producer: sending 1");
    co_await ch.send(1);
    LOG("Producer: sent 1 (buffered)");

    LOG("Producer: sending 2");
    co_await ch.send(2);
    LOG("Producer: sent 2 (buffered)");

    // This should block since buffer is full
    LOG("Producer: sending 3 (will block)");
    state.producer_block_start = get_now_ms();
    state.producer_blocked = true;
    co_await ch.send(3);
    state.producer_unblock_time = get_now_ms();
    LOG("Producer: sent 3 (was blocked for %d ms)", int(state.producer_unblock_time - state.producer_block_start));

    LOG("Producer: all operations completed");
    state.producer_completed = true;
    co_return;
  };

  // Consumer coroutine that tracks completion
  auto tracked_consumer = [](channel<int>& ch, channel_completion_state& state) -> async<void> {
    LOG("Consumer: sleep starting");
    co_await sleep(100ms);  // Let producer fill the buffer and start blocking
    LOG("Consumer: sleep completed, starting receive operations");

    // Verify producer is indeed blocked before we start consuming
    ASSERT(state.producer_blocked);
    ASSERT(!state.producer_completed);  // Producer should still be blocked

    auto val_opt = co_await ch.recv();
    ASSERT(val_opt.has_value());
    int val = *val_opt;
    LOG("Consumer: received %d", val);
    ASSERT(val == 1);

    val_opt = co_await ch.recv();
    ASSERT(val_opt.has_value());
    val = *val_opt;
    LOG("Consumer: received %d", val);
    ASSERT(val == 2);

    val_opt = co_await ch.recv();
    ASSERT(val_opt.has_value());
    val = *val_opt;
    LOG("Consumer: received %d", val);
    ASSERT(val == 3);

    LOG("Consumer: received all expected values");
    state.consumer_completed = true;
    LOG("Consumer: completion flag set");
    co_return;
  };

  co_await when_all(tracked_producer(ch, state), tracked_consumer(ch, state));

  LOG("About to check completion flags - consumer: %s, producer: %s", state.consumer_completed ? "yes" : "no",
      state.producer_completed ? "yes" : "no");

  // Verify timing: producer should have been unblocked by consumer
  ASSERT(state.producer_unblock_time - state.producer_block_start >= 100);
  LOG("Timing verified: producer unblocked(%d ms) after consumer started receiving", int(state.producer_unblock_time - state.producer_block_start));

  // Verify that both have indeed completed
  ASSERT(state.consumer_completed);
  ASSERT(state.producer_completed);
  LOG("Buffered channel test completed");
}

// Test channel close functionality
async<void> test_channel_close() {
  LOG("Starting channel close test");

  channel<int> ch;

  bool receiver_completed = false;
  auto receiver = [&ch, &receiver_completed]() -> async<void> {
    auto val_opt = co_await ch.recv();
    if (val_opt.has_value()) {
      LOG("Received value: %d", *val_opt);
    } else {
      LOG("Receiver got nullopt (channel closed)");
    }
    ASSERT(!val_opt.has_value());
    receiver_completed = true;
  };

  co_await spawn_local(receiver());

  // Allow receiver to start waiting
  co_await sleep(10ms);
  ch.close();

  // Allow some time for close to propagate
  co_await sleep(50ms);

  // Verify that receiver has indeed completed
  ASSERT(receiver_completed);
  LOG("Channel close test completed");
}

async<void> close_waiting_sender(channel<int>& ch, bool& send_result, bool& sender_completed) {
  send_result = co_await ch.send(7);
  sender_completed = true;
}

async<void> test_channel_close_unblocks_waiting_sender() {
  LOG("Starting channel close unblocks waiting sender test");

  channel<int> ch;
  bool send_result = true;
  bool sender_completed = false;

  co_await spawn_local(close_waiting_sender(ch, send_result, sender_completed));
  co_await sleep(10ms);

  ASSERT(!sender_completed);
  ch.close();
  co_await sleep(10ms);

  ASSERT(sender_completed);
  ASSERT(!send_result);

  bool immediate_send_result = co_await ch.send(8);
  ASSERT(!immediate_send_result);

  auto recv_result = co_await ch.recv();
  ASSERT(!recv_result.has_value());

  LOG("Channel close unblocks waiting sender test completed");
}

async<void> destroyed_channel_receiver(channel<int>* ch, bool& receiver_completed) {
  auto value = co_await ch->recv();
  ASSERT(!value.has_value());
  receiver_completed = true;
}

async<void> test_channel_destroy_unblocks_waiting_receiver() {
  LOG("Starting channel destroy unblocks waiting receiver test");

  auto ch = std::make_unique<channel<int>>();
  bool receiver_completed = false;

  co_await spawn_local(destroyed_channel_receiver(ch.get(), receiver_completed));
  co_await sleep(10ms);

  ASSERT(!receiver_completed);

  std::thread destroyer([owned = std::move(ch)]() mutable {
    owned.reset();
  });
  destroyer.join();

  ASSERT(!receiver_completed);
  co_await sleep(10ms);
  ASSERT(receiver_completed);

  LOG("Channel destroy unblocks waiting receiver test completed");
}

async<void> destroyed_channel_sender(channel<int>* ch, bool& send_result, bool& sender_completed) {
  send_result = co_await ch->send(9);
  sender_completed = true;
}

async<void> test_channel_destroy_unblocks_waiting_sender() {
  LOG("Starting channel destroy unblocks waiting sender test");

  auto ch = std::make_unique<channel<int>>();
  bool send_result = true;
  bool sender_completed = false;

  co_await spawn_local(destroyed_channel_sender(ch.get(), send_result, sender_completed));
  co_await sleep(10ms);

  ASSERT(!sender_completed);

  std::thread destroyer([owned = std::move(ch)]() mutable {
    owned.reset();
  });
  destroyer.join();

  ASSERT(!sender_completed);
  co_await sleep(10ms);
  ASSERT(sender_completed);
  ASSERT(!send_result);

  LOG("Channel destroy unblocks waiting sender test completed");
}

async<void> test_channel_drain_buffer_after_close() {
  LOG("Starting channel drain buffer after close test");

  channel<int> ch(2);

  ASSERT(co_await ch.send(1));
  ASSERT(co_await ch.send(2));
  ASSERT(ch.size() == 2);
  ASSERT(ch.full());

  ch.close();
  ASSERT(ch.is_closed());

  auto first = co_await ch.recv();
  ASSERT(first.has_value());
  ASSERT(*first == 1);

  auto second = co_await ch.recv();
  ASSERT(second.has_value());
  ASSERT(*second == 2);

  auto closed = co_await ch.recv();
  ASSERT(!closed.has_value());
  ASSERT(ch.empty());

  LOG("Channel drain buffer after close test completed");
}

async<void> queued_sender(channel<int>& ch, int value, int& waiting_count, bool& send_result, wait_group& completed) {
  ++waiting_count;
  send_result = co_await ch.send(value);
  completed.done();
}

async<void> test_unbuffered_channel_fifo_senders() {
  LOG("Starting unbuffered channel FIFO sender test");

  channel<int> ch;
  int waiting_count = 0;
  wait_group completed;
  completed.add(3);
  std::array<bool, 3> send_results{false, false, false};

  co_await spawn_local(queued_sender(ch, 1, waiting_count, send_results[0], completed));
  co_await spawn_local(queued_sender(ch, 2, waiting_count, send_results[1], completed));
  co_await spawn_local(queued_sender(ch, 3, waiting_count, send_results[2], completed));

  ASSERT(waiting_count == 3);

  auto first = co_await ch.recv();
  auto second = co_await ch.recv();
  auto third = co_await ch.recv();

  ASSERT(first.has_value());
  ASSERT(second.has_value());
  ASSERT(third.has_value());
  ASSERT(*first == 1);
  ASSERT(*second == 2);
  ASSERT(*third == 3);
  co_await completed;
  ASSERT(send_results[0]);
  ASSERT(send_results[1]);
  ASSERT(send_results[2]);

  LOG("Unbuffered channel FIFO sender test completed");
}

async<void> queued_receiver(channel<int>& ch, size_t index, int& waiting_count, std::array<int, 3>& received_values,
                            std::array<bool, 3>& receiver_completed, wait_group& completed) {
  ++waiting_count;
  auto value = co_await ch.recv();
  ASSERT(value.has_value());
  received_values[index] = *value;
  receiver_completed[index] = true;
  completed.done();
}

async<void> test_unbuffered_channel_fifo_receivers() {
  LOG("Starting unbuffered channel FIFO receiver test");

  channel<int> ch;
  int waiting_count = 0;
  wait_group completed;
  completed.add(3);
  std::array<int, 3> received_values{0, 0, 0};
  std::array<bool, 3> receiver_completed{false, false, false};

  co_await spawn_local(queued_receiver(ch, 0, waiting_count, received_values, receiver_completed, completed));
  co_await spawn_local(queued_receiver(ch, 1, waiting_count, received_values, receiver_completed, completed));
  co_await spawn_local(queued_receiver(ch, 2, waiting_count, received_values, receiver_completed, completed));

  ASSERT(waiting_count == 3);

  ASSERT(co_await ch.send(10));
  ASSERT(co_await ch.send(20));
  ASSERT(co_await ch.send(30));

  co_await completed;
  ASSERT(receiver_completed[0]);
  ASSERT(receiver_completed[1]);
  ASSERT(receiver_completed[2]);
  ASSERT(received_values[0] == 10);
  ASSERT(received_values[1] == 20);
  ASSERT(received_values[2] == 30);

  LOG("Unbuffered channel FIFO receiver test completed");
}

// Test ping-pong pattern with channels
async<void> test_ping_pong() {
  LOG("Starting ping-pong test");

  channel<int> ping_ch(1);
  channel<int> pong_ch(1);

  bool server_completed = false;
  bool client_completed = false;

  auto server = [&ping_ch, &pong_ch, &server_completed]() -> async<void> {
    while (true) {
      auto val_opt = co_await ping_ch.recv();
      if (!val_opt.has_value()) {
        LOG("Server: channel closed");
        server_completed = true;
        co_return;
      }

      int val = *val_opt;
      LOG("Server received ping: %d", val);

      if (val < 0) {
        LOG("Server terminating");
        server_completed = true;
        co_return;
      }

      bool ok = co_await pong_ch.send(val + 1);
      ASSERT(ok);
      LOG("Server sent pong: %d", val + 1);
    }
  };

  auto client = [&ping_ch, &pong_ch, &client_completed]() -> async<void> {
    bool ok = co_await ping_ch.send(1);
    ASSERT(ok);

    auto response_opt = co_await pong_ch.recv();
    ASSERT(response_opt.has_value());
    int response = *response_opt;
    LOG("Client received pong: %d", response);
    ASSERT(response == 2);

    ok = co_await ping_ch.send(response);
    ASSERT(ok);

    response_opt = co_await pong_ch.recv();
    ASSERT(response_opt.has_value());
    response = *response_opt;
    LOG("Client received pong: %d", response);
    ASSERT(response == 3);

    // Send negative number to terminate server
    ok = co_await ping_ch.send(-1);
    ASSERT(ok);
    LOG("Client sent termination signal");

    client_completed = true;
    co_return;
  };

  co_await spawn_local(server());
  co_await spawn_local(client());

  co_await sleep(100ms);

  // Verify that both have completed
  ASSERT(server_completed);
  ASSERT(client_completed);
  LOG("Ping-pong test completed");
}

// Test multiple producers and consumers
async<void> test_multiple_producers_consumers() {
  LOG("Starting multiple producers/consumers test");

  channel<int> ch(5);  // Buffered channel

  // Track completion of all coroutines
  bool producer1_completed = false;
  bool producer2_completed = false;
  bool consumer1_completed = false;
  bool consumer2_completed = false;

  // Producer that sends values 0-4
  auto producer = [](channel<int>& ch, int offset, bool* completed) -> async<void> {
    for (int i = 0; i < 5; ++i) {
      bool ok = co_await ch.send(offset * 10 + i);
      ASSERT(ok);
      LOG("Producer %d sent: %d", offset, offset * 10 + i);
      co_await sleep(5ms);  // Small delay between sends
    }
    *completed = true;
    co_return;
  };

  // Consumer that receives 5 values
  auto consumer = [](channel<int>& ch, int id, bool* completed) -> async<void> {
    for (int i = 0; i < 5; ++i) {
      auto val_opt = co_await ch.recv();
      ASSERT(val_opt.has_value());
      int val = *val_opt;
      LOG("Consumer %d received: %d", id, val);

      // Add some basic validation - values should be in expected range
      ASSERT(val >= 10 && val <= 24);  // Expected range from our 2 producers
    }
    *completed = true;
    co_return;
  };

  // Wait for all workers and their coroutine frames before inspecting shared state.
  co_await when_all(producer(ch, 1, &producer1_completed), producer(ch, 2, &producer2_completed), consumer(ch, 1, &consumer1_completed),
                    consumer(ch, 2, &consumer2_completed));

  LOG("Verify that all coroutines have completed");
  ASSERT(producer1_completed);
  ASSERT(producer2_completed);
  ASSERT(consumer1_completed);
  ASSERT(consumer2_completed);
  LOG("Multiple producers/consumers test completed");
}

async<void> stall_channel_test_loop() {
  co_await sleep(25ms);
  std::this_thread::sleep_for(250ms);
}

async<void> test_channel_completion_after_loop_stall() {
  co_await when_all(test_unbuffered_channel(), stall_channel_test_loop());
  co_await when_all(test_buffered_channel(), stall_channel_test_loop());
}

async<void> run_all_tests() {
  co_await test_channel_completion_after_loop_stall();
  co_await test_reused_send_results<channel_mt<int>>();
  co_await test_reused_send_results<channel_st<int>>();
  co_await test_reused_receive_results<channel_mt<int>>();
  co_await test_reused_receive_results<channel_st<int>>();
  co_await test_buffered_receive_reservation<channel_mt<int>>();
  co_await test_buffered_receive_reservation<channel_st<int>>();
#ifndef CORO_DISABLE_EXCEPTION
  co_await test_failed_send_keeps_receiver<channel_mt<throwing_channel_value>>();
  co_await test_failed_send_keeps_receiver<channel_st<throwing_channel_value>>();
  co_await test_failed_receive_keeps_sender<channel_mt<throwing_channel_value>>(false);
  co_await test_failed_receive_keeps_sender<channel_st<throwing_channel_value>>(false);
  co_await test_failed_receive_keeps_sender<channel_mt<throwing_channel_value>>(true);
  co_await test_failed_receive_keeps_sender<channel_st<throwing_channel_value>>(true);
  for (bool queued_close : {false, true}) {
    co_await test_failed_receive_extraction<channel_mt<throwing_channel_value>>(queued_close);
    co_await test_failed_receive_extraction<channel_st<throwing_channel_value>>(queued_close);
  }
  for (bool close_before_retry : {false, true}) {
    co_await test_reused_receive_after_refill_failure<channel_mt<throwing_channel_value>>(close_before_retry);
    co_await test_reused_receive_after_refill_failure<channel_st<throwing_channel_value>>(close_before_retry);
  }
  for (size_t capacity : {1, 2}) {
    for (bool close_before_receive : {false, true}) {
      co_await test_failed_refill_preserves_sender_order<channel_mt<throwing_channel_value>>(capacity, close_before_receive);
      co_await test_failed_refill_preserves_sender_order<channel_st<throwing_channel_value>>(capacity, close_before_receive);
    }
  }
#endif
  co_await test_unbuffered_channel();
  LOG("Unbuffered channel test passed");

  co_await test_buffered_channel();
  LOG("Buffered channel test passed");

  co_await test_channel_close();
  LOG("Channel close test passed");

  co_await test_channel_close_unblocks_waiting_sender();
  LOG("Channel close unblocks waiting sender test passed");

  co_await test_channel_destroy_unblocks_waiting_receiver();
  LOG("Channel destroy unblocks waiting receiver test passed");

  co_await test_channel_destroy_unblocks_waiting_sender();
  LOG("Channel destroy unblocks waiting sender test passed");

  co_await test_channel_drain_buffer_after_close();
  LOG("Channel drain buffer after close test passed");

  co_await test_unbuffered_channel_fifo_senders();
  LOG("Unbuffered channel FIFO sender test passed");

  co_await test_unbuffered_channel_fifo_receivers();
  LOG("Unbuffered channel FIFO receiver test passed");

  co_await test_ping_pong();
  LOG("Ping-pong test passed");

  co_await test_multiple_producers_consumers();
  LOG("Multiple producers/consumers test passed");
}

int main() {
  test_receive_lock_count_and_close();
  test_channel_awaiter_value_moves<channel_mt<counted_channel_value>>();
  test_channel_awaiter_value_moves<channel_st<counted_channel_value>>();
  test_channel_owner_selection();
  test_shared_ptr_st_lifetime();
  for (bool option : {false, true}) {
    test_channel_unstarted_awaiter_lifetime(option);
    test_channel_pending_awaiter_lifetime(option);
  }
  test_channel_lock_failure();
  check_registration_lifetime(channel_receiver_registration_lifetime);
  check_registration_lifetime(channel_sender_registration_lifetime);
  for (bool stopped : {false, true}) {
    test_stopped_loop_channel_resumption<channel_mt<int>>(stopped);
    test_stopped_loop_channel_resumption<channel_st<int>>(stopped);
  }
  test_channel_wakeup_chain<channel_mt<int>>();
  test_channel_wakeup_chain<channel_st<int>>();
  test_unbuffered_channel_does_not_allocate_message_storage<channel_mt>();
  test_unbuffered_channel_does_not_allocate_message_storage<channel_st>();
  test_receive_result_reentrancy<channel_mt<reentrant_channel_value>>();
  test_receive_result_reentrancy<channel_st<reentrant_channel_value>>();
  LOG("Channel test init");
  executor_loop executor;
  run_all_tests().detach(executor, [&] {
    executor.stop();
  });
  LOG("loop...");
  executor.run_loop();
  check_coro_leak();
  return 0;
}
