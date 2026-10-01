#define CORO_DEBUG_PROMISE_LEAK
#define CORO_DEBUG_ASYNC_STACK
#define CORO_ENABLE_LOCAL_STORAGE
#define CORO_DEBUG_LEAK_LOG test_log
#define CORO_DEBUG_LIFECYCLE test_log

void test_log(const char*, ...) noexcept;

#include "coro.hpp"

#ifdef CORO_TEST_WITH_ASIO
#include "coro/adaptor/asio_adaptor.hpp"
#endif

#ifdef CORO_DISABLE_EXCEPTION
template <typename T>
concept has_exception_state = requires(T value) { value.exception_; };

template <typename T>
concept has_unlock_exception = requires(T value) { value.unlock_exception_; };

static_assert(!has_exception_state<coro::awaitable_promise_value<void>>);
static_assert(!has_exception_state<coro::callback_awaiter<int>>);
static_assert(std::is_same_v<decltype(coro::awaitable_promise_value<int>::value_), std::optional<int>>);
static_assert(!has_unlock_exception<coro::condition_variable_st::wait_awaitable>);
static_assert(noexcept(std::declval<coro::condition_variable_st::wait_awaitable&>().await_resume()));
#endif
