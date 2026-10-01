#pragma once

// Keep replacement allocation probes opaque to optimizers. Inlining their malloc/free
// implementation into a new/delete call site can produce GCC's mismatched-allocation false positive.
#if defined(_MSC_VER)
#define CORO_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define CORO_TEST_NOINLINE [[gnu::noinline]]
#else
#define CORO_TEST_NOINLINE
#endif
