#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifdef CORO_BENCH_POSIX_RESOURCES
#include <sys/resource.h>
#endif

#include <async_simple/coro/Lazy.h>

#include <asio.hpp>

#ifdef CORO_BENCH_ALLOCATIONS
#include "allocation_counter.hpp"
#endif

#include "coro/adaptor/asio_adaptor.hpp"
#include "coro/executor_loop.hpp"

#if defined(_MSC_VER)
#define CORO_BENCH_NOINLINE __declspec(noinline)
#else
#define CORO_BENCH_NOINLINE __attribute__((noinline))
#endif

namespace {
using value_type = std::uint64_t;
using clock_type = std::chrono::steady_clock;

// Keep the recursive coroutine boundary in optimized builds for all three libraries.
CORO_BENCH_NOINLINE coro::async<value_type> coro_call(value_type value, unsigned depth) {
  if (depth == 0) {
    co_return value;
  }
  co_return co_await coro_call(value, depth - 1);
}

CORO_BENCH_NOINLINE async_simple::coro::Lazy<value_type> simple_call(value_type value, unsigned depth) {
  if (depth == 0) {
    co_return value;
  }
  co_return co_await simple_call(value, depth - 1);
}

CORO_BENCH_NOINLINE asio::awaitable<value_type> asio_call(value_type value, unsigned depth) {
  if (depth == 0) {
    co_return value;
  }
  co_return co_await asio_call(value, depth - 1);
}

enum class workload { immediate, nested, post };

coro::async<value_type> coro_work(value_type count, workload work) {
  value_type sum = 0;
  for (value_type i = 1; i <= count; ++i) {
    if (work == workload::post) {
      co_await coro::callback_awaiter<void>([](auto executor, auto callback) {
        executor->post(std::move(callback));
      });
      sum += i;
    } else {
      sum += co_await coro_call(i, work == workload::nested ? 8 : 0);
    }
  }
  co_return sum;
}

async_simple::coro::Lazy<value_type> simple_work(value_type count, workload work) {
  value_type sum = 0;
  for (value_type i = 1; i <= count; ++i) {
    if (work == workload::post) {
      co_await async_simple::coro::Yield{};
      sum += i;
    } else {
      sum += co_await simple_call(i, work == workload::nested ? 8 : 0);
    }
  }
  co_return sum;
}

asio::awaitable<value_type> asio_work(value_type count, workload work) {
  value_type sum = 0;
  for (value_type i = 1; i <= count; ++i) {
    if (work == workload::post) {
      co_await asio::post(asio::use_awaitable);
      sum += i;
    } else {
      sum += co_await asio_call(i, work == workload::nested ? 8 : 0);
    }
  }
  co_return sum;
}

// Benchmark-local adapter: one io_context, one run thread, no thread pool.
class simple_asio_executor : public async_simple::Executor {
 public:
  explicit simple_asio_executor(asio::io_context& context) : context_(context) {}

  bool schedule(Func fn) override {
    asio::post(context_, std::move(fn));
    return true;
  }

  bool currentThreadInExecutor() const override {
    return context_.get_executor().running_in_this_thread();
  }

 private:
  asio::io_context& context_;
};

// Diagnostic controls: distinguish coroutine overhead from Asio executor type erasure.
class concrete_coro_asio_executor final : public coro::asio_executor {
 public:
  explicit concrete_coro_asio_executor(asio::io_context& context) : coro::asio_executor(context), context_(context) {}

  void dispatch(std::function<void()> fn) override {
    asio::dispatch(context_, std::move(fn));
  }
  void post(std::function<void()> fn) override {
    asio::post(context_, std::move(fn));
  }

 private:
  asio::io_context& context_;
};

class erased_simple_asio_executor final : public simple_asio_executor {
 public:
  explicit erased_simple_asio_executor(asio::io_context& context) : simple_asio_executor(context), executor_(context.get_executor()) {}

  bool schedule(Func fn) override {
    asio::post(executor_, std::move(fn));
    return true;
  }

 private:
  asio::any_io_executor executor_;
};

constexpr std::array library_names{"coro", "async_simple", "Asio", "coro_native", "coro_asio_concrete", "async_simple_asio_erased"};
constexpr std::array primary_libraries{0u, 1u, 2u};
constexpr std::array diagnostic_libraries{0u, 1u, 2u, 3u, 4u, 5u};
constexpr std::array workload_names{"immediate", "nested_8", "post"};

template <typename EXECUTOR>
void start_coro(EXECUTOR& executor, value_type count, workload work, value_type& sum, bool& completed, std::exception_ptr& error) {
  coro_work(count, work)
      .detach_with_callback(
          executor,
          [&](value_type result) {
            sum = result;
            completed = true;
          },
          [&](std::exception_ptr failure) {
            error = failure;
            completed = true;
          });
}

value_type run(unsigned library, value_type count, workload work) {
  value_type sum = 0;
  bool completed = false;
  std::exception_ptr error;
  if (library == 3) {
    coro::executor_loop executor;
    coro_work(count, work)
        .detach_with_callback(
            executor,
            [&](value_type result) {
              sum = result;
              completed = true;
              executor.stop();
            },
            [&](std::exception_ptr failure) {
              error = failure;
              completed = true;
              executor.stop();
            });
    executor.run_loop();
  } else {
    asio::io_context context;
    coro::asio_executor coro_executor(context);
    simple_asio_executor simple_executor(context);
    concrete_coro_asio_executor concrete_executor(context);
    erased_simple_asio_executor erased_executor(context);
    if (library == 0) {
      start_coro(coro_executor, count, work, sum, completed, error);
    } else if (library == 4) {
      start_coro(concrete_executor, count, work, sum, completed, error);
    } else if (library == 1 || library == 5) {
      auto* executor = library == 1 ? &simple_executor : &erased_executor;
      simple_work(count, work).via(executor).start([&](async_simple::Try<value_type>&& result) {
        if (result.hasError()) {
          error = result.getException();
        } else {
          sum = result.value();
        }
        completed = true;
      });
    } else {
      asio::co_spawn(context, asio_work(count, work), [&](std::exception_ptr failure, value_type result) {
        error = failure;
        sum = result;
        completed = true;
      });
    }
    context.run();
  }
  if (error) {
    std::rethrow_exception(error);
  }
  if (!completed || sum != count * (count + 1) / 2) {
    throw std::runtime_error("workload did not complete with the expected checksum");
  }
  return sum;
}

value_type parse(std::string_view text, value_type limit) {
  value_type value = 0;
  auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0 || value > limit) {
    throw std::invalid_argument("argument must be a positive integer within the documented limit");
  }
  return value;
}

#ifdef CORO_BENCH_ALLOCATIONS
void allocation_self_test() {
  allocation_counter::begin();
  void* first = ::operator new(7);
  void* array = ::operator new[](11, std::nothrow);
  void* aligned = ::operator new(64, std::align_val_t{64});
  void* aligned_array = ::operator new[](128, std::align_val_t{128}, std::nothrow);
  const bool aligned_ok = reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0 && reinterpret_cast<std::uintptr_t>(aligned_array) % 128 == 0;
  ::operator delete(first);
  ::operator delete[](array, std::nothrow);
  ::operator delete(aligned, std::align_val_t{64});
  ::operator delete[](aligned_array, std::align_val_t{128}, std::nothrow);
  const auto stats = allocation_counter::end();
  if (!aligned_ok || stats.calls != 4 || stats.requested != 210 || stats.peak != 210 || stats.live != 0) {
    throw std::runtime_error("allocation counter self-test failed");
  }
  // Older scopes and zero-sized allocations must not underflow live bytes.
  void* older = ::operator new(3);
  allocation_counter::begin();
  void* zero = ::operator new(0);
  ::operator delete(older);
  ::operator delete(zero);
  const auto next = allocation_counter::end();
  if (next.calls != 1 || next.requested != 0 || next.live != 0) {
    throw std::runtime_error("allocation counter scope isolation failed");
  }
}
#endif

#ifdef CORO_BENCH_POSIX_RESOURCES
long long microseconds(timeval value) {
  return static_cast<long long>(value.tv_sec) * 1000000 + value.tv_usec;
}

void resource_run(unsigned library, unsigned work, value_type count) {
  rusage before{}, after{};
  if (getrusage(RUSAGE_SELF, &before) != 0) {
    throw std::runtime_error("getrusage failed");
  }
#ifdef CORO_BENCH_ALLOCATIONS
  allocation_counter::begin();
#endif
  const auto sum = run(library, count, static_cast<workload>(work));
#ifdef CORO_BENCH_ALLOCATIONS
  const auto allocation = allocation_counter::end();
#endif
  if (getrusage(RUSAGE_SELF, &after) != 0) {
    throw std::runtime_error("getrusage failed");
  }
  auto rss = static_cast<long long>(after.ru_maxrss);
#ifndef __APPLE__
  rss *= 1024;
#endif
  const auto& names = library_names;
  const auto& work_names = workload_names;
  std::cout << "| workload | library | iterations | user_cpu_us | system_cpu_us | peak_rss_bytes | checksum";
#ifdef CORO_BENCH_ALLOCATIONS
  std::cout << " | new_calls | new_requested_bytes | new_peak_live_bytes | new_retained_bytes";
#endif
  std::cout << " |\n|---|---|---:|---:|---:|---:|---:";
#ifdef CORO_BENCH_ALLOCATIONS
  std::cout << "|---:|---:|---:|---:";
#endif
  std::cout << "|\n| " << work_names[work] << " | " << names[library] << " | " << count << " | "
            << microseconds(after.ru_utime) - microseconds(before.ru_utime) << " | " << microseconds(after.ru_stime) - microseconds(before.ru_stime)
            << " | " << rss << " | " << sum;
#ifdef CORO_BENCH_ALLOCATIONS
  std::cout << " | " << allocation.calls << " | " << allocation.requested << " | " << allocation.peak << " | " << allocation.live;
#endif
  std::cout << " |\n";
}
#endif
}  // namespace

int main(int argc, char** argv) {
  try {
#ifdef CORO_BENCH_ALLOCATIONS
    allocation_self_test();
#endif
#ifdef CORO_BENCH_POSIX_RESOURCES
    if (argc == 5 && std::string_view(argv[1]) == "--resource") {
      // Reuse positive-integer parsing while allowing zero-based case indices.
      const std::string_view library_text(argv[2]), work_text(argv[3]);
      const auto library = library_text == "0" ? 0 : parse(library_text, library_names.size() - 1);
      const auto work = work_text == "0" ? 0 : parse(work_text, 2);
      resource_run(static_cast<unsigned>(library), static_cast<unsigned>(work), parse(argv[4], 1000000000));
      return 0;
    }
#endif
    const bool diagnostics = argc > 1 && std::string_view(argv[1]) == "--diagnostics";
    const int first_argument = diagnostics ? 2 : 1;
    if (argc - first_argument > 2) {
      throw std::invalid_argument("usage: coro_benchmark [--diagnostics] [iterations<=1000000000] [rounds<=1000]");
    }
    const auto count = argc > first_argument ? parse(argv[first_argument], 1000000000) : 100000;
    const auto rounds = argc > first_argument + 1 ? parse(argv[first_argument + 1], 1000) : 7;
    const std::span<const unsigned> libraries =
        diagnostics ? std::span<const unsigned>(diagnostic_libraries) : std::span<const unsigned>(primary_libraries);
    const auto& names = library_names;
    constexpr std::array works{workload::immediate, workload::nested, workload::post};
    const auto& work_names = workload_names;
    std::cout << "# Coroutine benchmark\n\n";
#ifdef CORO_ENABLE_FRAME_CACHE
    std::cout << "coro frame cache: enabled (" << CORO_FRAME_CACHE_SLOTS << " slots, at most " << CORO_FRAME_CACHE_MAX_BYTES
              << " bytes per frame).\n\n";
#endif
    std::cout << "| workload | library | iterations | rounds | median_ns_per_op | min_ns_per_op | max_ns_per_op | checksum |\n"
                 "|---|---|---:|---:|---:|---:|---:|---:|\n";
    std::cout << std::fixed << std::setprecision(2);
    for (unsigned w = 0; w < works.size(); ++w) {
      std::array<std::vector<double>, library_names.size()> samples;
      for (auto lib : libraries) {
        run(lib, std::min<value_type>(count, 10000), works[w]);
      }
      for (value_type round = 0; round < rounds; ++round) {
        // Rotate order to reduce systematic warm-cache / thermal ordering bias.
        for (unsigned offset = 0; offset < libraries.size(); ++offset) {
          const auto lib = libraries[(round + offset) % libraries.size()];
          const auto begin = clock_type::now();
          run(lib, count, works[w]);
          const auto elapsed = std::chrono::duration<double, std::nano>(clock_type::now() - begin).count();
          samples[lib].push_back(elapsed / static_cast<double>(count));
        }
      }
      for (auto lib : libraries) {
        auto& values = samples[lib];
        std::sort(values.begin(), values.end());
        const double median = (values[(values.size() - 1) / 2] + values[values.size() / 2]) / 2;
        std::cout << "| " << work_names[w] << " | " << names[lib] << " | " << count << " | " << rounds << " | " << median << " | " << values.front()
                  << " | " << values.back() << " | " << count * (count + 1) / 2 << " |\n";
      }
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
