#pragma once

#ifdef CCT_DISABLE_SPDLOG
#include <string>
#include <string_view>
#else
#include <spdlog/fmt/fmt.h>
#endif

namespace aeronet {

#ifdef CCT_DISABLE_SPDLOG

template <typename... Args>
using format_string = std::string_view;

template <typename OutputIt, typename... T>
auto format_to(OutputIt out, format_string<T...>, T&&...) {
  return out;
}

template <typename OutputIt, typename... T>
auto format_to_n(OutputIt out, [[maybe_unused]] std::iter_difference_t<OutputIt> n, format_string<T...>, T&&...) {
  struct res {
    OutputIt out;
    std::iter_difference_t<OutputIt> size;
  };
  return res{out, n};
}

template <typename... T>
auto format(format_string<T...> fmt, [[maybe_unused]] T&&... args) {
  return std::string{fmt};
}

#else
template <typename... Args>
using format_string = fmt::format_string<Args...>;
using fmt::format;
using fmt::format_to;
using fmt::format_to_n;
#endif

}  // namespace aeronet
