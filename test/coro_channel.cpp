/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include <array>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

#include "log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include "assert_def.h"
#include "coro.hpp"
#include "utils.hpp"

using namespace coro;

thread_local size_t* channel_allocation_bytes = nullptr;

void* operator new(std::size_t size) {
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

void operator delete(void* memory) noexcept {
  std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
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
    CHANNEL ch;
    ASSERT(ch.empty());
    ASSERT(!ch.full());
    ASSERT(ch.size() == 0);
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
async<void> reused_queued_sender(CHANNEL& ch, bool& completed) {
  auto sending = ch.send(42);
  ASSERT(co_await sending);
  ASSERT(!(co_await sending));
  completed = true;
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
    bool completed = false;
    co_await spawn_local(reused_queued_sender(ch, completed));
    ASSERT(!completed);
    ch.close();
    ASSERT(completed);
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
      std::atomic<bool> completed{false};
      std::thread worker([&] {
        auto result = receiving.await_resume();
        ASSERT(result && result->state == &state);
        completed.store(true, std::memory_order_release);
      });
      auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!completed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
      }
      ASSERT(completed.load(std::memory_order_acquire));
      worker.join();
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
async<void> reservation_receiver(CHANNEL& ch, int& result, bool& completed) {
  auto value = co_await ch.recv();
  ASSERT(value);
  result = *value;
  completed = true;
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
  bool second_completed = false;
  int second_value = 0;
  co_await spawn_local(reservation_receiver(ch, second_value, second_completed));
  auto first_value = first.await_resume();
  ASSERT(first_value && *first_value == 1);
  ASSERT(!second_completed);
  co_await ch.send(2);
  ASSERT(second_completed);
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
async<void> transfer_receiver(CHANNEL& ch, bool& completed) {
  auto value = co_await ch.recv();
  ASSERT(!value);
  completed = true;
}

template <typename CHANNEL>
async<void> transfer_sender(CHANNEL& ch, transfer_state& state, int value, bool& completed) {
  bool delivered = co_await ch.send(throwing_channel_value{state, value});
  ASSERT(!delivered);
  completed = true;
}

template <typename CHANNEL>
async<void> test_failed_send_keeps_receiver() {
  transfer_state state;
  CHANNEL ch;
  bool receiver_completed = false;
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
    ASSERT(receiver_completed);
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
  bool sender_completed = false;
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
  ASSERT(sender_completed);
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
  bool sender_completed = false;
  auto sender = [](CHANNEL& ch, transfer_state& state, bool& completed, bool expected) -> async<void> {
    ASSERT((co_await ch.send(throwing_channel_value{state, 42})) == expected);
    completed = true;
  };
  co_await spawn_local(sender(ch, state, sender_completed, !close_before_retry));
  ASSERT(!sender_completed);
  auto receiving = ch.recv();
  state.fail_value = 42;
  bool caught = false;
  try {
    co_await receiving;
  } catch (const std::runtime_error&) {
    caught = true;
  }
  ASSERT(caught);
  ASSERT(!sender_completed);
  state.fail_value = -1;
  if (close_before_retry) {
    ch.close();
    ASSERT(!(co_await receiving));
  } else {
    auto value = co_await receiving;
    ASSERT(value && value->value == 42);
  }
  ASSERT(sender_completed);
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
  auto sender = [](CHANNEL& ch, transfer_state& state, int value, bool expected, bool& completed) -> async<void> {
    ASSERT((co_await ch.send(throwing_channel_value{state, value})) == expected);
    completed = true;
  };
  bool first_completed = false;
  bool second_completed = false;
  co_await spawn_local(sender(ch, state, 42, !close_before_receive, first_completed));
  ASSERT(!first_completed);
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
  ASSERT(!first_completed);
  ASSERT(!second_completed);
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
  ASSERT(first_completed);
  ASSERT(second_completed);
  ASSERT(!(co_await ch.recv()));
  ASSERT(state.live == 0);
}
#endif

// Test basic unbuffered channel
async<void> test_unbuffered_channel() {
  LOG("Starting unbuffered channel test");

  channel<int> ch;  // unbuffered channel

  // Track completion and timing
  bool consumer_completed = false;
  bool producer_completed = false;
  bool producer_blocked = false;
  uint64_t producer_block_start = 0;
  uint64_t producer_unblock_time = 0;

  // Producer coroutine that tracks completion and blocking
  auto tracked_producer = [&]() -> async<void> {
    LOG("Producer: sending 42 (will block until consumer ready)");
    producer_block_start = get_now_ms();
    producer_blocked = true;
    co_await ch.send(42);
    producer_unblock_time = get_now_ms();
    LOG("Producer: sent 42 (was blocked for %d ms)", int(producer_unblock_time - producer_block_start));

    LOG("Producer: sending 100 (will block until consumer ready)");
    co_await ch.send(100);
    LOG("Producer: sent 100");

    producer_completed = true;
    co_return;
  };

  // Consumer coroutine that tracks completion
  auto tracked_consumer = [&]() -> async<void> {
    // Delay to ensure producer starts first and gets blocked
    co_await sleep(50ms);

    // Verify producer is blocked before we start receiving
    ASSERT(producer_blocked);
    ASSERT(!producer_completed);

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
    consumer_completed = true;
    co_return;
  };

  // Start producer first (it should block immediately)
  co_await spawn_local(tracked_producer());
  co_await spawn_local(tracked_consumer());

  // Allow time for operations to complete
  co_await sleep(100ms);

  // Verify timing: producer should have been unblocked by consumer
  ASSERT(producer_unblock_time - producer_block_start);
  LOG("Timing verified: producer was unblocked(%d ms) after consumer started receiving", int(producer_unblock_time - producer_block_start));

  // Verify that both have completed
  ASSERT(consumer_completed);
  ASSERT(producer_completed);
  LOG("Unbuffered channel test completed");
}

// Test basic buffered channel
async<void> test_buffered_channel() {
  LOG("Starting buffered channel test");

  channel<int> ch(2);  // buffered channel with capacity 2

  // Track completion and timing
  bool consumer_completed = false;
  bool producer_completed = false;
  bool producer_blocked = false;
  uint64_t producer_block_start = 0;
  uint64_t producer_unblock_time = 0;

  // Producer coroutine that tracks completion and blocking
  auto tracked_producer = [&]() -> async<void> {
    LOG("Producer: sending 1");
    co_await ch.send(1);
    LOG("Producer: sent 1 (buffered)");

    LOG("Producer: sending 2");
    co_await ch.send(2);
    LOG("Producer: sent 2 (buffered)");

    // This should block since buffer is full
    LOG("Producer: sending 3 (will block)");
    producer_block_start = get_now_ms();
    producer_blocked = true;
    co_await ch.send(3);
    producer_unblock_time = get_now_ms();
    LOG("Producer: sent 3 (was blocked for %d ms)", int(producer_unblock_time - producer_block_start));

    LOG("Producer: all operations completed");
    producer_completed = true;
    co_return;
  };

  // Consumer coroutine that tracks completion
  auto tracked_consumer = [&]() -> async<void> {
    LOG("Consumer: sleep starting");
    co_await sleep(100ms);  // Let producer fill the buffer and start blocking
    LOG("Consumer: sleep completed, starting receive operations");

    // Verify producer is indeed blocked before we start consuming
    ASSERT(producer_blocked);
    ASSERT(!producer_completed);  // Producer should still be blocked

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
    consumer_completed = true;
    LOG("Consumer: completion flag set");
    co_return;
  };

  co_await spawn_local(tracked_producer());
  co_await spawn_local(tracked_consumer());

  // Wait for operations to complete
  co_await sleep(200ms);

  LOG("About to check completion flags - consumer: %s, producer: %s", consumer_completed ? "yes" : "no", producer_completed ? "yes" : "no");

  // Verify timing: producer should have been unblocked by consumer
  ASSERT(producer_unblock_time - producer_block_start >= 100);
  LOG("Timing verified: producer unblocked(%d ms) after consumer started receiving", int(producer_unblock_time - producer_block_start));

  // Verify that both have indeed completed
  ASSERT(consumer_completed);
  ASSERT(producer_completed);
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

async<void> queued_sender(channel<int>& ch, int value, int& waiting_count, bool& send_result) {
  ++waiting_count;
  send_result = co_await ch.send(value);
}

async<void> test_unbuffered_channel_fifo_senders() {
  LOG("Starting unbuffered channel FIFO sender test");

  channel<int> ch;
  int waiting_count = 0;
  std::array<bool, 3> send_results{false, false, false};

  co_await spawn_local(queued_sender(ch, 1, waiting_count, send_results[0]));
  co_await spawn_local(queued_sender(ch, 2, waiting_count, send_results[1]));
  co_await spawn_local(queued_sender(ch, 3, waiting_count, send_results[2]));

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
  ASSERT(send_results[0]);
  ASSERT(send_results[1]);
  ASSERT(send_results[2]);

  LOG("Unbuffered channel FIFO sender test completed");
}

async<void> queued_receiver(channel<int>& ch, size_t index, int& waiting_count, std::array<int, 3>& received_values,
                            std::array<bool, 3>& receiver_completed) {
  ++waiting_count;
  auto value = co_await ch.recv();
  ASSERT(value.has_value());
  received_values[index] = *value;
  receiver_completed[index] = true;
}

async<void> test_unbuffered_channel_fifo_receivers() {
  LOG("Starting unbuffered channel FIFO receiver test");

  channel<int> ch;
  int waiting_count = 0;
  std::array<int, 3> received_values{0, 0, 0};
  std::array<bool, 3> receiver_completed{false, false, false};

  co_await spawn_local(queued_receiver(ch, 0, waiting_count, received_values, receiver_completed));
  co_await spawn_local(queued_receiver(ch, 1, waiting_count, received_values, receiver_completed));
  co_await spawn_local(queued_receiver(ch, 2, waiting_count, received_values, receiver_completed));

  ASSERT(waiting_count == 3);

  ASSERT(co_await ch.send(10));
  ASSERT(co_await ch.send(20));
  ASSERT(co_await ch.send(30));

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

async<void> run_all_tests() {
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
  test_unbuffered_channel_does_not_allocate_message_storage<channel_mt>();
  test_unbuffered_channel_does_not_allocate_message_storage<channel_st>();
  test_receive_result_reentrancy<channel_mt<reentrant_channel_value>>();
  test_receive_result_reentrancy<channel_st<reentrant_channel_value>>();
  LOG("Channel test init");
  executor_loop executor;
  run_all_tests().detach_with_callback(executor, [&] {
    executor.stop();
  });
  LOG("loop...");
  executor.run_loop();
  check_coro_leak();
  return 0;
}
