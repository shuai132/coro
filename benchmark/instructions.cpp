#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>

#ifdef CORO_INSTRUCTIONS_CACHEGRIND
#include <valgrind/cachegrind.h>
#endif

#include "coro.hpp"

#ifdef CORO_INSTRUCTIONS_ALLOCATIONS
#include "allocation_counter.hpp"
#endif

#ifdef CORO_INSTRUCTIONS_CHECK
#include "assert_def.h"
#include "log.h"
#include "utils.hpp"
#endif

namespace {
using namespace coro;
using value_type = std::uint64_t;

async<value_type> value_task(value_type value, unsigned depth = 0) {
  if (depth) {
    co_return co_await value_task(value, depth - 1);
  }
  co_return value;
}

async<void> post_once() {
  co_await callback_awaiter<void>([](executor* exec, auto callback) {
    exec->post(std::move(callback));
  });
}

async<void> task_case(value_type count, value_type& sum) {
  for (value_type i = 1; i <= count; ++i) {
    sum += co_await value_task(i);
  }
}

async<void> nested_case(value_type count, value_type& sum) {
  for (value_type i = 1; i <= count; ++i) {
    sum += co_await value_task(i, 8);
  }
}

async<void> callback_sync_case(value_type count, value_type& sum) {
  for (value_type i = 1; i <= count; ++i) {
    sum += co_await callback_awaiter<value_type>([i](auto callback) {
      callback(i);
    });
  }
}

async<void> callback_post_case(value_type count, value_type& sum) {
  for (value_type i = 1; i <= count; ++i) {
    co_await post_once();
    sum += i;
  }
}

async<void> send_messages(channel<value_type>& channel, value_type count) {
  for (value_type i = 1; i <= count; ++i) {
    co_await channel.send(i);
  }
  channel.close();
}

async<void> receive_messages(channel<value_type>& channel, value_type& sum) {
  while (auto value = co_await channel.recv()) {
    sum += *value;
  }
}

template <std::size_t CAPACITY>
async<void> channel_case(value_type count, value_type& sum) {
  channel<value_type> channel(CAPACITY);
  co_await when_all(send_messages(channel, count), receive_messages(channel, sum));
}

template <typename WAIT>
async<void> wait_and_count(WAIT wait, value_type& sum, value_type value, bool& resumed) {
  co_await wait();
  resumed = true;
  sum += value;
}

template <typename SIGNAL>
async<void> signal_after_post(SIGNAL signal, const bool& resumed) {
  // Both when_all children must start before signaling, so the other child really waits.
  co_await post_once();
  if (resumed) {
    std::abort();
  }
  signal();
}

async<void> mutex_case(value_type count, value_type& sum) {
  mutex mutex;
  for (value_type i = 1; i <= count; ++i) {
    co_await mutex.lock();
    bool resumed = false;
    co_await when_all(wait_and_count(
                          [&] {
                            return mutex.scoped_lock();
                          },
                          sum, i, resumed),
                      signal_after_post(
                          [&] {
                            mutex.unlock();
                          },
                          resumed));
  }
}

async<void> semaphore_case(value_type count, value_type& sum) {
  counting_semaphore semaphore(0);
  for (value_type i = 1; i <= count; ++i) {
    bool resumed = false;
    co_await when_all(wait_and_count(
                          [&] {
                            return semaphore.acquire();
                          },
                          sum, i, resumed),
                      signal_after_post(
                          [&] {
                            semaphore.release();
                          },
                          resumed));
  }
}

async<void> event_case(value_type count, value_type& sum) {
  event event;
  for (value_type i = 1; i <= count; ++i) {
    bool resumed = false;
    co_await when_all(wait_and_count(
                          [&] {
                            return event.wait();
                          },
                          sum, i, resumed),
                      signal_after_post(
                          [&] {
                            event.set();
                          },
                          resumed));
    event.clear();
  }
}

async<void> condition_wait(condition_variable& cv, mutex& mutex, value_type& sum, value_type value, bool& resumed) {
  auto guard = co_await mutex.scoped_lock();
  co_await cv.wait(mutex);
  co_await mutex.lock();
  resumed = true;
  sum += value;
}

async<void> condition_variable_case(value_type count, value_type& sum) {
  condition_variable cv;
  mutex mutex;
  for (value_type i = 1; i <= count; ++i) {
    bool resumed = false;
    co_await when_all(condition_wait(cv, mutex, sum, i, resumed), signal_after_post(
                                                                      [&] {
                                                                        cv.notify_one();
                                                                      },
                                                                      resumed));
  }
}

async<void> latch_case(value_type count, value_type& sum) {
  for (value_type i = 1; i <= count; ++i) {
    latch latch(1);
    bool resumed = false;
    co_await when_all(wait_and_count(
                          [&] {
                            return latch.wait();
                          },
                          sum, i, resumed),
                      signal_after_post(
                          [&] {
                            latch.count_down();
                          },
                          resumed));
  }
}

async<void> wait_group_case(value_type count, value_type& sum) {
  wait_group group;
  for (value_type i = 1; i <= count; ++i) {
    group.add(1);
    bool resumed = false;
    co_await when_all(wait_and_count(
                          [&] {
                            return group.wait();
                          },
                          sum, i, resumed),
                      signal_after_post(
                          [&] {
                            group.done();
                          },
                          resumed));
  }
}

async<value_type> posted_value(value_type value, unsigned posts, wait_group& finished) {
  for (unsigned i = 0; i < posts; ++i) {
    co_await post_once();
  }
  finished.done();
  co_return value;
}

async<void> when_all_case(value_type count, value_type& sum) {
  for (value_type i = 1; i <= count; ++i) {
    wait_group finished;
    finished.add(2);
    auto [first, second] = co_await when_all(posted_value(i, 1, finished), posted_value(i, 2, finished));
    sum += first == i && second == i ? i : 0;
  }
}

async<void> when_any_case(value_type count, value_type& sum) {
  for (value_type i = 1; i <= count; ++i) {
    wait_group finished;
    finished.add(2);
    auto winner = co_await when_any(posted_value(i, 1, finished), posted_value(i, 2, finished));
    sum += winner.index == 0 ? winner.get<0>() : winner.get<1>();
    co_await finished;
  }
}

struct scenario {
  const char* name_;
  async<void> (*work_)(value_type, value_type&);
};

constexpr std::array scenarios{
    scenario{"task", task_case},
    scenario{"nested", nested_case},
    scenario{"callback_sync", callback_sync_case},
    scenario{"callback_post", callback_post_case},
    scenario{"channel_buffered", channel_case<64>},
    scenario{"channel_unbuffered", channel_case<0>},
    scenario{"mutex", mutex_case},
    scenario{"semaphore", semaphore_case},
    scenario{"event", event_case},
    scenario{"condition_variable", condition_variable_case},
    scenario{"latch", latch_case},
    scenario{"wait_group", wait_group_case},
    scenario{"when_all", when_all_case},
    scenario{"when_any", when_any_case},
};

// Keep setup, task destruction and executor destruction within this call's measurement boundary.
[[gnu::noinline]] value_type run(const scenario& selected, value_type count) {
  executor_poll executor;
  value_type sum = 0;
  bool done = false;
  selected.work_(count, sum)
      .detach(
          executor,
          [&] {
            done = true;
          },
          [](std::exception_ptr) {
            std::abort();
          });
  while (!done) {
    executor.poll();
  }
  return sum;
}

bool valid(value_type sum, value_type count) {
  return sum == count * (count + 1) / 2;
}

#ifdef CORO_INSTRUCTIONS_CHECK
struct move_only_value {
  value_type value_;
  explicit move_only_value(value_type initial) : value_(initial) {}
  move_only_value(move_only_value&&) = default;
  move_only_value(const move_only_value&) = delete;
};

async<void> check_callbacks() {
  auto value = co_await callback_awaiter<move_only_value>([](executor* exec, auto callback) {
    exec->post([callback = std::move(callback)] {
      callback(move_only_value{42});
    });
  });
  ASSERT(value.value_ == 42);
  auto immediate = co_await callback_awaiter<move_only_value>([](auto callback) {
    callback(move_only_value{43});
  });
  ASSERT(immediate.value_ == 43);
  co_await callback_awaiter<void>([](auto callback) {
    callback();
  });
}

int check() {
  for (const auto& selected : scenarios) {
    for (value_type count : {0, 1, 17}) {
      ASSERT(valid(run(selected, count), count));
      check_coro_leak();
    }
  }
  executor_poll executor;
  bool done = false;
  check_callbacks().detach(executor, [&] {
    done = true;
  });
  while (!done) {
    executor.poll();
  }
  check_coro_leak();
  return 0;
}
#endif

#ifdef CORO_INSTRUCTIONS_ALLOCATIONS
int check_allocations() {
  void* older = ::operator new(3);
  allocation_counter::begin();
  ::operator delete(older);
  void* first = ::operator new(7);
  void* array = ::operator new[](11, std::nothrow);
  void* aligned = ::operator new(64, std::align_val_t{64});
  void* aligned_array = ::operator new[](128, std::align_val_t{128}, std::nothrow);
  const bool aligned_ok = reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0 && reinterpret_cast<std::uintptr_t>(aligned_array) % 128 == 0;
  ::operator delete(first);
  const auto stats = allocation_counter::end();
  if (!array || !aligned_array || !aligned_ok || stats.calls != 4 || stats.requested != 210 || stats.peak != 210 || stats.live != 203) {
    std::abort();
  }
  ::operator delete(aligned, std::align_val_t{64});
  ::operator delete[](aligned_array, std::align_val_t{128}, std::nothrow);

  // Freeing a prior interval's allocation must not subtract from this interval's live bytes.
  allocation_counter::begin();
  void* zero = ::operator new(0);
  ::operator delete[](array, std::nothrow);
  ::operator delete(zero);
  const auto next = allocation_counter::end();
  if (next.calls != 1 || next.requested != 0 || next.peak != 0 || next.live != 0) {
    std::abort();
  }
  return 0;
}
#endif

// Verification aid: work and output here must not enter the measured interval.
void outside_work(value_type count) {
  if (count) {
#ifdef CORO_INSTRUCTIONS_ALLOCATIONS
    void* allocation = ::operator new(static_cast<std::size_t>(count));
#endif
    volatile value_type sum = 0;
    for (value_type i = 0; i < count; ++i) {
      sum = sum + i;
    }
    std::fprintf(stderr, "Outside region: %llu\n", static_cast<unsigned long long>(sum));
#ifdef CORO_INSTRUCTIONS_ALLOCATIONS
    ::operator delete(allocation);
#endif
  }
}

bool parse(const char* text, value_type& result) {
  std::string_view input(text);
  const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size() && result <= 1000000;
}
}  // namespace

int main(int argc, char** argv) {
#ifdef CORO_INSTRUCTIONS_ALLOCATIONS
  if (argc == 2 && std::string_view(argv[1]) == "--check-allocations") {
    return check_allocations();
  }
#endif
#ifdef CORO_INSTRUCTIONS_CHECK
  if (argc == 2 && std::string_view(argv[1]) == "--check") {
    return check();
  }
#endif
  if (argc == 2 && std::string_view(argv[1]) == "--list") {
    for (const auto& selected : scenarios) {
      std::puts(selected.name_);
    }
    return 0;
  }
  value_type count = 0;
  value_type outside = 0;
  if ((argc != 3 && argc != 4) || !parse(argv[2], count) || (argc == 4 && !parse(argv[3], outside))) {
    std::fputs("usage: coro_instructions SCENARIO ITERATIONS [OUTSIDE_WORK]\n", stderr);
    return 1;
  }
  for (const auto& selected : scenarios) {
    if (selected.name_ != std::string_view(argv[1])) {
      continue;
    }
    if (!valid(run(selected, 100), 100)) {
      return 1;
    }
    outside_work(outside);
#ifdef CORO_INSTRUCTIONS_CACHEGRIND
    CACHEGRIND_START_INSTRUMENTATION;
#endif
#ifdef CORO_INSTRUCTIONS_ALLOCATIONS
    allocation_counter::begin();
#endif
    const auto sum = run(selected, count);
#ifdef CORO_INSTRUCTIONS_ALLOCATIONS
    const auto allocations = allocation_counter::end();
#endif
#ifdef CORO_INSTRUCTIONS_CACHEGRIND
    CACHEGRIND_STOP_INSTRUMENTATION;
#endif
    outside_work(outside);
    if (!valid(sum, count)) {
      return 1;
    }
#ifdef CORO_INSTRUCTIONS_ALLOCATIONS
    std::printf(
        "{\"scenario\":\"%s\",\"iterations\":%llu,\"checksum\":%llu,"
        "\"new_calls\":%zu,\"new_requested_bytes\":%zu,\"new_peak_live_bytes\":%zu,\"new_live_bytes\":%zu}\n",
        selected.name_, static_cast<unsigned long long>(count), static_cast<unsigned long long>(sum), allocations.calls, allocations.requested,
        allocations.peak, allocations.live);
#else
    std::printf("%s checksum=%llu\n", selected.name_, static_cast<unsigned long long>(sum));
#endif
    return 0;
  }
  std::fputs("Unknown scenario\n", stderr);
  return 1;
}
