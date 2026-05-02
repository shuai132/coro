#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <numeric>
#include <type_traits>

#include "coro.hpp"

namespace coro {

using namespace std::chrono_literals;

namespace detail {

inline uint64_t scale_duration_remainder(uint64_t value, uint64_t numerator, uint64_t denominator, uint64_t* remainder_out = nullptr) noexcept {
  if (value <= std::numeric_limits<uint64_t>::max() / numerator) {
    auto product = value * numerator;
    if (remainder_out) {
      *remainder_out = product % denominator;
    }
    return product / denominator;
  }
  // Track the quotient and remainder separately when the product does not fit.
  uint64_t quotient = 0;
  uint64_t remainder = 0;
  for (uint64_t bit = uint64_t{1} << 63; bit; bit >>= 1) {
    quotient *= 2;
    remainder *= 2;
    if (remainder >= denominator) {
      remainder -= denominator;
      ++quotient;
    }
    if (numerator & bit) {
      remainder += value;
      if (remainder >= denominator) {
        remainder -= denominator;
        ++quotient;
      }
    }
  }
  if (remainder_out) {
    *remainder_out = remainder;
  }
  return quotient;
}

template <typename REP, typename PERIOD>
uint64_t sleep_duration_ns(std::chrono::duration<REP, PERIOD> duration) {
  constexpr auto max_ns = static_cast<uint64_t>(std::chrono::nanoseconds::max().count());
  if (!(duration > duration.zero())) {
    return 0;
  }
  constexpr auto divisor = std::gcd(PERIOD::den, std::nano::den);
  constexpr auto factor = std::nano::den / divisor;
  constexpr bool wide_ratio = PERIOD::num > std::numeric_limits<std::intmax_t>::max() / factor;
  if constexpr (std::numeric_limits<REP>::is_integer) {
    auto count = static_cast<std::common_type_t<REP, uint64_t>>(duration.count());
    if constexpr (wide_ratio) {
      // Split seconds and subsecond remainder without forming an overflowing ratio.
      constexpr auto numerator = static_cast<uint64_t>(PERIOD::num);
      constexpr auto denominator = static_cast<uint64_t>(PERIOD::den);
      constexpr uint64_t ns_per_second = 1000000000;
      constexpr auto max_seconds = max_ns / ns_per_second;
      auto whole = count / denominator;
      if (whole > max_seconds / numerator) {
        return max_ns;
      }
      auto seconds = static_cast<uint64_t>(whole) * numerator;
      uint64_t remainder;
      auto fractional_seconds = scale_duration_remainder(static_cast<uint64_t>(count % denominator), numerator, denominator, &remainder);
      if (fractional_seconds > max_seconds - seconds) {
        return max_ns;
      }
      auto ns = (seconds + fractional_seconds) * ns_per_second;
      auto fractional_ns = scale_duration_remainder(remainder, ns_per_second, denominator);
      return fractional_ns > max_ns - ns ? max_ns : ns + fractional_ns;
    } else {
      using scale = std::ratio_divide<PERIOD, std::nano>;
      constexpr auto numerator = static_cast<uint64_t>(scale::num);
      constexpr auto denominator = static_cast<uint64_t>(scale::den);
      auto whole = count / denominator;
      if (whole > max_ns / numerator) {
        return max_ns;
      }
      auto ns = static_cast<uint64_t>(whole) * numerator;
      auto fractional = scale_duration_remainder(static_cast<uint64_t>(count % denominator), numerator, denominator);
      return fractional > max_ns - ns ? max_ns : ns + fractional;
    }
  } else {
    using calculation_rep = std::common_type_t<REP, std::chrono::nanoseconds::rep, std::intmax_t>;
    calculation_rep ns;
    if constexpr (wide_ratio) {
      constexpr auto numerator = static_cast<calculation_rep>(PERIOD::num) * static_cast<calculation_rep>(factor);
      ns = static_cast<calculation_rep>(duration.count()) * numerator / static_cast<calculation_rep>(PERIOD::den / divisor);
    } else {
      ns = std::chrono::duration_cast<std::chrono::duration<calculation_rep, std::nano>>(duration).count();
    }
    if (ns >= static_cast<calculation_rep>(max_ns)) {
      return max_ns;
    }
    return static_cast<uint64_t>(static_cast<std::chrono::nanoseconds::rep>(ns));
  }
}

}  // namespace detail

template <typename Rep, typename Period>
inline callback_awaiter<void> sleep(std::chrono::duration<Rep, Period> duration) {
  auto ns = detail::sleep_duration_ns(duration);
  return callback_awaiter<void>([ns](auto executor, auto callback) {
    executor->post_delayed_ns(std::move(callback), ns);
  });
}

inline callback_awaiter<void> delay(uint64_t ms) {
  using nanoseconds = std::chrono::nanoseconds;
  constexpr auto max_ns = static_cast<uint64_t>(nanoseconds::max().count());
  auto ns = ms > max_ns / 1000000 ? max_ns : ms * 1000000;
  return sleep(nanoseconds(static_cast<nanoseconds::rep>(ns)));
}

template <typename Rep, typename Period>
inline auto operator co_await(std::chrono::duration<Rep, Period> duration) {
  return sleep(duration);
}

}  // namespace coro
