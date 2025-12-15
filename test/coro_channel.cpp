/// config debug
#define CORO_DEBUG_PROMISE_LEAK
// #define CORO_DISABLE_EXCEPTION
#include "log.h"
// #define CORO_DEBUG_LEAK_LOG LOG
// #define CORO_DEBUG_LIFECYCLE LOG

#include "assert_def.h"
#include "coro.hpp"
#include "utils.hpp"

using namespace coro;

uint64_t get_now_ms() {
  auto now = std::chrono::steady_clock::now();
  auto epoch = now.time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
}

// Test basic unbuffered channel
async<void> test_unbuffered_channel(executor& exec) {
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
  co_spawn(exec, tracked_producer());
  co_spawn(exec, tracked_consumer());

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
async<void> test_buffered_channel(executor& exec) {
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

  co_spawn(exec, tracked_producer());
  co_spawn(exec, tracked_consumer());

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
async<void> test_channel_close(executor& exec) {
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

  co_spawn(exec, receiver());

  // Allow receiver to start waiting
  co_await sleep(10ms);
  ch.close();

  // Allow some time for close to propagate
  co_await sleep(50ms);

  // Verify that receiver has indeed completed
  ASSERT(receiver_completed);
  LOG("Channel close test completed");
}

// Test ping-pong pattern with channels
async<void> test_ping_pong(executor& exec) {
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

  co_spawn(exec, server());
  co_spawn(exec, client());

  co_await sleep(100ms);

  // Verify that both have completed
  ASSERT(server_completed);
  ASSERT(client_completed);
  LOG("Ping-pong test completed");
}

// Test multiple producers and consumers
async<void> test_multiple_producers_consumers(executor& exec) {
  LOG("Starting multiple producers/consumers test");

  channel<int> ch(5);  // Buffered channel

  // Track completion of all coroutines
  bool producer1_completed = false;
  bool producer2_completed = false;
  bool consumer1_completed = false;
  bool consumer2_completed = false;

  // Producer that sends values 0-4
  auto producer = [&ch](int offset, bool* completed) -> async<void> {
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
  auto consumer = [&ch](int id, bool* completed) -> async<void> {
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

  // Start 2 producers and 2 consumers
  co_spawn(exec, producer(1, &producer1_completed));
  co_spawn(exec, producer(2, &producer2_completed));
  co_spawn(exec, consumer(1, &consumer1_completed));
  co_spawn(exec, consumer(2, &consumer2_completed));

  co_await sleep(200ms);

  LOG("Verify that all coroutines have completed");
  ASSERT(producer1_completed);
  ASSERT(producer2_completed);
  ASSERT(consumer1_completed);
  ASSERT(consumer2_completed);
  LOG("Multiple producers/consumers test completed");
}

async<void> run_all_tests(executor& exec) {
  co_await test_unbuffered_channel(exec);
  LOG("Unbuffered channel test passed");

  co_await test_buffered_channel(exec);
  LOG("Buffered channel test passed");

  co_await test_channel_close(exec);
  LOG("Channel close test passed");

  co_await test_ping_pong(exec);
  LOG("Ping-pong test passed");

  co_await test_multiple_producers_consumers(exec);
  LOG("Multiple producers/consumers test passed");
}

int main() {
  LOG("Channel test init");
  executor_loop executor;
  co_spawn(executor, run_all_tests(executor));
  auto debug = debug_and_stop(executor, 1500);
  LOG("loop...");
  executor.run_loop();
  debug.join();
  return 0;
}
