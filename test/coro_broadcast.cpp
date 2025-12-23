/// Test for broadcast channel functionality
#define CORO_DEBUG_PROMISE_LEAK
#include "assert_def.h"
#include "coro.hpp"
#include "log.h"
#include "utils.hpp"

using namespace coro;

// Test basic broadcast to multiple receivers
async<void> test_broadcast_multiple_receivers() {
  LOG("Starting broadcast test with multiple receivers");

  channel<int> ch;  // unbuffered channel

  int receiver1_value = 0;
  int receiver2_value = 0;
  int receiver3_value = 0;
  bool receiver1_done = false;
  bool receiver2_done = false;
  bool receiver3_done = false;

  // Create multiple receivers
  auto receiver = [&ch](int id, int* value_ptr, bool* done_ptr) -> async<void> {
    LOG("Receiver %d: waiting for broadcast", id);
    auto val_opt = co_await ch.recv();
    if (val_opt.has_value()) {
      *value_ptr = *val_opt;
      LOG("Receiver %d: received value %d", id, *value_ptr);
    } else {
      LOG("Receiver %d: channel closed", id);
    }
    *done_ptr = true;
    co_return;
  };

  auto& exec = *co_await current_executor();

  // Start 3 receivers
  co_spawn(exec, receiver(1, &receiver1_value, &receiver1_done));
  co_spawn(exec, receiver(2, &receiver2_value, &receiver2_done));
  co_spawn(exec, receiver(3, &receiver3_value, &receiver3_done));

  // Give receivers time to start waiting
  co_await sleep(50ms);

  // Broadcast a value - should go to ALL receivers
  LOG("Broadcasting value 42 to all receivers");
  size_t count = co_await ch.broadcast(42);
  LOG("Broadcast notified %zu receivers", count);

  ASSERT(count == 3);  // Should have notified all 3 receivers

  // Wait for all receivers to complete
  co_await sleep(50ms);

  // Verify all receivers got the same value
  ASSERT(receiver1_done);
  ASSERT(receiver2_done);
  ASSERT(receiver3_done);
  ASSERT(receiver1_value == 42);
  ASSERT(receiver2_value == 42);
  ASSERT(receiver3_value == 42);

  LOG("Broadcast test completed successfully - all 3 receivers got the value");
}

// Test broadcast vs send - verify send only goes to one receiver
async<void> test_broadcast_vs_send() {
  LOG("Starting broadcast vs send comparison test");

  channel<int> ch;

  int receiver1_value = 0;
  int receiver2_value = 0;
  bool receiver1_done = false;
  bool receiver2_done = false;

  auto receiver = [&ch](int id, int* value_ptr, bool* done_ptr) -> async<void> {
    auto val_opt = co_await ch.recv();
    if (val_opt.has_value()) {
      *value_ptr = *val_opt;
      LOG("Receiver %d: received value %d", id, *value_ptr);
    }
    *done_ptr = true;
    co_return;
  };

  auto& exec = *co_await current_executor();

  // Start 2 receivers
  co_spawn(exec, receiver(1, &receiver1_value, &receiver1_done));
  co_spawn(exec, receiver(2, &receiver2_value, &receiver2_done));

  co_await sleep(50ms);

  // First, use send() - should only go to ONE receiver
  LOG("Using send(100) - should go to only one receiver");
  bool send_ok = co_await ch.send(100);
  ASSERT(send_ok);

  co_await sleep(50ms);

  // Exactly one receiver should have received the value
  int received_count = (receiver1_value == 100 ? 1 : 0) + (receiver2_value == 100 ? 1 : 0);
  ASSERT(received_count == 1);
  LOG("send() correctly delivered to only 1 receiver");

  // Now broadcast to remaining receiver
  LOG("Using broadcast(200) - should go to the remaining receiver");
  size_t broadcast_count = co_await ch.broadcast(200);
  ASSERT(broadcast_count == 1);  // Only one receiver waiting

  co_await sleep(50ms);

  // Both receivers should now be done
  ASSERT(receiver1_done);
  ASSERT(receiver2_done);

  // One should have 100, the other should have 200
  bool has_100 = (receiver1_value == 100 || receiver2_value == 100);
  bool has_200 = (receiver1_value == 200 || receiver2_value == 200);
  ASSERT(has_100);
  ASSERT(has_200);

  LOG("Broadcast vs send test completed successfully");
}

// Test broadcast with no receivers
async<void> test_broadcast_no_receivers() {
  LOG("Starting broadcast with no receivers test");

  channel<int> ch;

  // Broadcast with no one waiting - should return 0
  LOG("Broadcasting with no receivers waiting");
  size_t count = co_await ch.broadcast(42);
  ASSERT(count == 0);
  LOG("Correctly returned 0 receivers notified");

  LOG("Broadcast with no receivers test completed");
}

// Test sequential broadcasts
async<void> test_sequential_broadcasts() {
  LOG("Starting sequential broadcasts test");

  channel<int> ch;

  std::vector<int> receiver1_values;
  std::vector<int> receiver2_values;

  auto receiver = [&ch](int id, std::vector<int>* values) -> async<void> {
    for (int i = 0; i < 3; i++) {
      auto val_opt = co_await ch.recv();
      if (val_opt.has_value()) {
        values->push_back(*val_opt);
        LOG("Receiver %d: received value %d (broadcast %d)", id, *val_opt, i + 1);
      }
    }
    co_return;
  };

  auto& exec = *co_await current_executor();

  co_spawn(exec, receiver(1, &receiver1_values));
  co_spawn(exec, receiver(2, &receiver2_values));

  co_await sleep(50ms);

  // Send multiple broadcasts
  for (int i = 1; i <= 3; i++) {
    LOG("Broadcasting value %d", i * 10);
    size_t count = co_await ch.broadcast(i * 10);
    ASSERT(count == 2);    // Both receivers should get it
    co_await sleep(30ms);  // Give time for receivers to process
  }

  co_await sleep(50ms);

  // Both receivers should have received all 3 values
  ASSERT(receiver1_values.size() == 3);
  ASSERT(receiver2_values.size() == 3);

  // Verify the values
  for (int i = 0; i < 3; i++) {
    ASSERT(receiver1_values[i] == (i + 1) * 10);
    ASSERT(receiver2_values[i] == (i + 1) * 10);
  }

  LOG("Sequential broadcasts test completed successfully");
}

void run_multithread_tests() {
  LOG("=== Starting multi-threaded broadcast tests ===\n");

  // Test 1: Broadcast to multiple receivers across threads
  {
    LOG("Test 1: Broadcast across multiple threads");
    channel_mt<int> ch;
    std::atomic<int> receiver_count(0);

    std::vector<executor_loop> executors(4);  // Pre-allocate: 3 receivers + 1 broadcaster
    std::vector<std::thread> threads;

    // Create 3 receiver executors and detach coroutines first
    for (int i = 0; i < 3; i++) {
      auto& exec = executors[i];

      auto receiver = [](channel_mt<int>& ch, std::atomic<int>& receiver_count, int id) -> async<void> {
        LOG("Receiver %d: waiting", id);
        auto val_opt = co_await ch.recv();
        if (val_opt.has_value()) {
          receiver_count.fetch_add(1);
          LOG("Receiver %d: received %d", id, *val_opt);
        }
        co_return;
      };

      receiver(ch, receiver_count, i + 1).detach(exec);
    }

    // Now start threads for receiver executors
    threads.reserve(3);
    for (size_t i = 0; i < 3; i++) {
      threads.emplace_back([&exec = executors[i]]() {
        exec.run_loop();
      });
    }

    // Wait for receivers to start
    std::this_thread::sleep_for(100ms);

    // Use broadcaster executor
    auto& exec_broadcast = executors[3];

    auto broadcaster = [](channel_mt<int>& ch, std::vector<executor_loop>& executors) -> async<void> {
      LOG("Broadcasting 999");
      size_t count = co_await ch.broadcast(999);
      LOG("Broadcast notified %zu receivers", count);
      ASSERT(count == 3);

      // Give time for receivers to process
      co_await sleep(100ms);

      // Stop all executors
      for (auto& exec : executors) {
        exec.stop();
      }
      co_return;
    };

    broadcaster(ch, executors).detach_with_callback(exec_broadcast, [&exec_broadcast]() {
      exec_broadcast.stop();
    });

    threads.emplace_back([&exec_broadcast]() {
      exec_broadcast.run_loop();
    });

    // Wait for all threads
    for (auto& t : threads) {
      t.join();
    }

    ASSERT(receiver_count.load() == 3);
    LOG("✓ Test 1 passed\n");
  }

  // Test 2: Multiple broadcasts across threads
  {
    LOG("Test 2: Multiple broadcasts across threads");
    channel_mt<int> ch;
    std::atomic<int> total_received(0);

    std::vector<executor_loop> executors(3);  // Pre-allocate: 2 receivers + 1 broadcaster
    std::vector<std::thread> threads;

    // Create 2 receiver executors and detach coroutines first
    for (int i = 0; i < 2; i++) {
      auto& exec = executors[i];

      auto receiver = [](channel_mt<int>& ch, std::atomic<int>& total_received, int id) -> async<void> {
        for (int j = 0; j < 3; j++) {
          auto val_opt = co_await ch.recv();
          if (val_opt.has_value()) {
            total_received.fetch_add(1);
            LOG("Receiver %d: received %d", id, *val_opt);
          }
        }
        co_return;
      };

      receiver(ch, total_received, i + 1).detach(exec);
    }

    // Now start threads for receiver executors
    threads.reserve(2);
    for (size_t i = 0; i < 2; i++) {
      threads.emplace_back([&exec = executors[i]]() {
        exec.run_loop();
      });
    }

    // Wait for receivers to start
    std::this_thread::sleep_for(100ms);

    // Use broadcaster executor
    auto& exec_broadcast = executors[2];

    auto broadcaster = [](channel_mt<int>& ch, std::vector<executor_loop>& executors) -> async<void> {
      // Send 3 broadcasts
      for (int i = 0; i < 3; i++) {
        int value = (i + 1) * 100;
        LOG("Broadcasting %d", value);
        size_t count = co_await ch.broadcast(value);
        LOG("Broadcast %d notified %zu receivers", i, count);
        co_await sleep(50ms);
      }

      // Give time for receivers to process
      co_await sleep(100ms);

      // Stop all executors
      for (auto& exec : executors) {
        exec.stop();
      }
      co_return;
    };

    broadcaster(ch, executors).detach_with_callback(exec_broadcast, [&exec_broadcast]() {
      exec_broadcast.stop();
    });

    threads.emplace_back([&exec_broadcast]() {
      exec_broadcast.run_loop();
    });

    // Wait for all threads
    for (auto& t : threads) {
      t.join();
    }

    ASSERT(total_received.load() == 6);  // 2 receivers * 3 broadcasts
    LOG("✓ Test 2 passed\n");
  }

  LOG("=== All multi-threaded tests passed! ===");
}

async<void> run_all_tests() {
  co_await test_broadcast_multiple_receivers();
  LOG("✓ Broadcast multiple receivers test passed\n");

  co_await test_broadcast_vs_send();
  LOG("✓ Broadcast vs send test passed\n");

  co_await test_broadcast_no_receivers();
  LOG("✓ Broadcast no receivers test passed\n");

  co_await test_sequential_broadcasts();
  LOG("✓ Sequential broadcasts test passed\n");

  LOG("=== All single-threaded broadcast tests passed! ===");
}

int main() {
  LOG("Broadcast channel test init");
  executor_loop executor;
  run_all_tests().detach_with_callback(executor, [&] {
    executor.stop();
  });
  LOG("loop...");
  executor.run_loop();

  // Run multithreaded tests
  run_multithread_tests();

  check_coro_leak();
  return 0;
}
