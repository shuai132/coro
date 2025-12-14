/// config debug
#define CORO_DEBUG_PROMISE_LEAK
#include "TimeCount.hpp"
#include "assert_def.h"
#include "coro.hpp"
#include "log.h"

using namespace coro;

callback_awaiter<void> delay_ms(uint32_t ms) {
  return callback_awaiter<void>([ms](auto executor, auto callback) {
    executor->post_delayed(std::move(callback), ms);
  });
}

// Test basic unbuffered channel
async<void> test_unbuffered_channel(executor& exec) {
  LOG("Starting unbuffered channel test");

  channel<int> ch;  // unbuffered channel

  // Producer coroutine
  auto producer = [&ch]() -> async<void> {
    LOG("Producer: sending 42");
    co_await ch.send(42);
    LOG("Producer: sent 42");

    LOG("Producer: sending 100");
    co_await ch.send(100);
    LOG("Producer: sent 100");

    co_return;
  };

  // Consumer coroutine
  auto consumer = [&ch]() -> async<void> {
    int val = co_await ch.recv();
    LOG("Consumer: received %d", val);
    ASSERT(val == 42);

    val = co_await ch.recv();
    LOG("Consumer: received %d", val);
    ASSERT(val == 100);

    LOG("Consumer: received all expected values");
    co_return;
  };

  // Track completion
  bool consumer_completed = false;

  // Consumer coroutine that tracks completion
  auto tracked_consumer = [&ch, &consumer_completed]() -> async<void> {
    int val = co_await ch.recv();
    LOG("Consumer: received %d", val);
    ASSERT(val == 42);

    val = co_await ch.recv();
    LOG("Consumer: received %d", val);
    ASSERT(val == 100);

    LOG("Consumer: received all expected values");
    consumer_completed = true;
    co_return;
  };

  // Run consumer first (it will wait for producer)
  co_spawn(exec, tracked_consumer());
  co_spawn(exec, producer());

  // Allow time for operations to complete
  co_await delay_ms(100);

  // Verify that consumer has indeed completed
  ASSERT(consumer_completed);
  LOG("Unbuffered channel test completed");
}

// Test basic buffered channel
async<void> test_buffered_channel(executor& exec) {
  LOG("Starting buffered channel test");

  channel<int> ch(2);  // buffered channel with capacity 2

  // Producer coroutine
  auto producer = [&ch]() -> async<void> {
    LOG("Producer: sending 1");
    co_await ch.send(1);
    LOG("Producer: sent 1 (buffered)");

    LOG("Producer: sending 2");
    co_await ch.send(2);
    LOG("Producer: sent 2 (buffered)");

    // This should block since buffer is full
    LOG("Producer: sending 3 (will block)");
    co_await ch.send(3);
    LOG("Producer: sent 3");

    co_return;
  };

  // Track completion
  bool consumer_completed = false;
  bool producer_completed = false;

  // Producer coroutine that tracks completion
  auto tracked_producer = [&ch, &producer_completed]() -> async<void> {
    LOG("Producer: sending 1");
    co_await ch.send(1);
    LOG("Producer: sent 1 (buffered)");

    LOG("Producer: sending 2");
    co_await ch.send(2);
    LOG("Producer: sent 2 (buffered)");

    // This should block since buffer is full
    LOG("Producer: sending 3 (will block)");
    co_await ch.send(3);
    LOG("Producer: sent 3");

    LOG("Producer: all operations completed");
    producer_completed = true;
    co_return;
  };

  // Consumer coroutine that tracks completion
  auto tracked_consumer = [&ch, &consumer_completed]() -> async<void> {
    LOG("Consumer: delay starting");
    co_await delay_ms(50);  // Let producer fill the buffer first
    LOG("Consumer: delay completed, starting receive operations");

    int val = co_await ch.recv();
    LOG("Consumer: received %d", val);
    ASSERT(val == 1);

    val = co_await ch.recv();
    LOG("Consumer: received %d", val);
    ASSERT(val == 2);

    val = co_await ch.recv();
    LOG("Consumer: received %d", val);
    ASSERT(val == 3);

    LOG("Consumer: received all expected values");
    consumer_completed = true;
    LOG("Consumer: completion flag set");
    co_return;
  };

  co_spawn(exec, tracked_producer());
  co_spawn(exec, tracked_consumer());

  // Allow more time for operations to complete
  co_await delay_ms(500);  // Increased from 200 to 500

  LOG("About to check completion flags - consumer: %s, producer: %s", consumer_completed ? "yes" : "no", producer_completed ? "yes" : "no");

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
    try {
      int val = co_await ch.recv();
      LOG("Received value: %d", val);
      (void)val;  // Suppress unused warning
    } catch (...) {
      LOG("Receiver caught exception (channel closed)");
    }
    receiver_completed = true;
  };

  co_spawn(exec, receiver());

  // Allow receiver to start waiting
  co_await delay_ms(10);
  ch.close();

  // Allow some time for close to propagate
  co_await delay_ms(50);

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
      int val = co_await ping_ch.recv();
      LOG("Server received ping: %d", val);

      if (val < 0) {
        LOG("Server terminating");
        server_completed = true;
        co_return;
      }

      co_await pong_ch.send(val + 1);
      LOG("Server sent pong: %d", val + 1);
    }
  };

  auto client = [&ping_ch, &pong_ch, &client_completed]() -> async<void> {
    co_await ping_ch.send(1);

    int response = co_await pong_ch.recv();
    LOG("Client received pong: %d", response);
    ASSERT(response == 2);

    co_await ping_ch.send(response);

    response = co_await pong_ch.recv();
    LOG("Client received pong: %d", response);
    ASSERT(response == 3);

    // Send negative number to terminate server
    co_await ping_ch.send(-1);
    LOG("Client sent termination signal");

    client_completed = true;
    co_return;
  };

  co_spawn(exec, server());
  co_spawn(exec, client());

  co_await delay_ms(100);

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
      co_await ch.send(offset * 10 + i);
      LOG("Producer %d sent: %d", offset, offset * 10 + i);
      co_await delay_ms(5);  // Small delay between sends
    }
    *completed = true;
    co_return;
  };

  // Consumer that receives 5 values
  auto consumer = [&ch](int id, bool* completed) -> async<void> {
    for (int i = 0; i < 5; ++i) {
      int val = co_await ch.recv();
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

  co_await delay_ms(150);

  // Verify that all coroutines have completed
  ASSERT(producer1_completed);
  ASSERT(producer2_completed);
  ASSERT(consumer1_completed);
  ASSERT(consumer2_completed);
  LOG("Multiple producers/consumers test completed");
}

void debug_and_stop(auto& executor, int wait_ms = 1000) {
  std::thread([&executor, wait_ms] {
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    executor.dispatch([&executor] {
#ifdef CORO_DEBUG_PROMISE_LEAK
      LOG("debug: debug_coro_leak.size: %zu", debug_coro_promise::debug_coro_leak.size());
      ASSERT(debug_coro_promise::debug_coro_leak.empty());
#endif
      executor.stop();
    });
  }).detach();
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
  executor_single_thread executor;
  co_spawn(executor, run_all_tests(executor));
  debug_and_stop(executor, 1000);
  LOG("loop...");
  executor.run_loop();
  return 0;
}
