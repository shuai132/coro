// Compile-only regression for standard libraries without native thread support.
#include "coro/channel.hpp"
#include "coro/condition_variable.hpp"
#include "coro/event.hpp"
#include "coro/latch.hpp"
#include "coro/semaphore.hpp"
#include "coro/wait_group.hpp"

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
