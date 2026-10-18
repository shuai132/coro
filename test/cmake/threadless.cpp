// Compile-only regression for standard libraries without native thread support.
#ifndef CORO_ENABLE_LOCAL_STORAGE
#define CORO_ENABLE_LOCAL_STORAGE
#endif

#include <type_traits>

#include "coro/channel.hpp"
#include "coro/condition_variable.hpp"
#include "coro/coro_local.hpp"
#include "coro/event.hpp"
#include "coro/latch.hpp"
#include "coro/semaphore.hpp"
#include "coro/wait_group.hpp"

#ifdef CORO_SINGLE_THREAD
#include "coro/when.hpp"
static_assert(std::is_same_v<decltype(std::declval<coro::async<void>::promise_type>().local_storage_),
                             coro::detail::shared_ptr_st<coro::detail::coro_local_map>>);

coro::async<int> single_thread_value() {
  co_return 42;
}

coro::async<void> single_thread_void() {
  co_return;
}

coro::async<void> instantiate_single_thread_combinators() {
  auto all = co_await coro::when_all(single_thread_value(), single_thread_value());
  auto any = co_await coro::when_any(single_thread_value(), single_thread_void());
  co_await coro::when_all(single_thread_void(), single_thread_void());
  auto any_void = co_await coro::when_any(single_thread_void(), single_thread_void());
  (void)all;
  (void)any;
  (void)any_void;
}
#endif

#ifdef CORO_TEST_NO_NATIVE_MUTEX
static_assert(!CORO_DETAIL_HAS_STD_MUTEX, "This regression must use a threadless standard library");
#endif

void instantiate_single_thread_primitives() {
  coro::channel_st<int> channel(1);
  coro::unbuffered_channel_st<int> unbuffered;
  coro::condition_variable_st condition;
  coro::event_st event(true);
  coro::latch_st latch(0);
  coro::counting_semaphore_st counting(1);
  coro::binary_semaphore_st binary(1);
  coro::wait_group_st group;
  channel.close();
  condition.notify_all();
  event.clear();
  counting.try_acquire();
  binary.try_acquire();
  group.add(1);
  group.done();
}

coro::async<int> instantiate_single_thread_waiters(coro::channel_st<int>& channel, coro::event_st& event, coro::latch_st& latch,
                                                   coro::counting_semaphore_st& semaphore, coro::binary_semaphore_st& binary,
                                                   coro::wait_group_st& group, coro::condition_variable_st& condition) {
  coro::coro_local<int> value;
  auto initial = co_await value.get_optional();
  co_await value.set(initial.value_or(42));
  co_await value.has();
  co_await semaphore.acquire();
  semaphore.release();
  co_await binary.acquire();
  binary.release();
  co_await event;
  co_await latch;
  co_await group;
  coro::dummy_mutex mutex;
  mutex.lock();
  co_await condition.wait(mutex);
  mutex.lock();
  mutex.unlock();
  co_await channel.send(co_await value.get());
  co_await channel.broadcast(7);
  auto received = co_await channel.recv();
  co_await value.erase();
  co_return received.value_or(0);
}
