#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "aeronet/base-fd.hpp"
#include "aeronet/dynamic-concatenated-strings.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

// Lightweight DogStatsD client
// Sends Datadog-compatible StatsD messages over a Unix domain datagram socket (UDS).
// Usage:
//   #include "dogstatsd.hpp"
//   DogStatsD statsd("/var/run/datadog/dsd.socket", "myapp");
//   statsd.increment("requests.processed", 1, {"route:home", "env:prod"});
// Notes:
//  - Uses SOCK_DGRAM AF_UNIX; requires Datadog Agent configured to listen on that socket.
//  - If socket creation/send fails, construction of the object throws.
//  - This is intentionally minimal and dependency-free; it focuses on non-blocking sends and simple
//    formatting (tags as dogstatsd format: |#tag1,tag2:value).
//  - It is thread-safe to call the metric methods from multiple threads concurrently.
class DogStatsD {
 private:
  static constexpr const char kCommaSep[] = ",";

 public:
  using DogStatsDTags = DynamicConcatenatedStrings<kCommaSep, false, uint32_t>;

  // socketPath: path to unix datagram socket used by the agent (e.g. /var/run/datadog/dsd.socket)
  // ns: optional metric namespace prefix (e.g. "myapp.")
  // Disables the client if socketPath is empty.
  explicit DogStatsD(std::string_view socketPath = {}, std::string_view ns = {},
                     std::chrono::milliseconds connectTimeout = std::chrono::milliseconds{5000});

  void increment(std::string_view metric, uint64_t value = 1UL, const DogStatsDTags& tags = {}) const noexcept;

  void gauge(std::string_view metric, int64_t value, const DogStatsDTags& tags = {}) const noexcept;

  void histogram(std::string_view metric, double value, const DogStatsDTags& tags = {}) const noexcept;

  void timing(std::string_view metric, std::chrono::milliseconds ms, const DogStatsDTags& tags = {}) const noexcept;

  void set(std::string_view metric, std::string_view value, const DogStatsDTags& tags = {}) const noexcept;

 private:
  void sendMetricMessage(std::string_view metric, std::string_view value, std::string_view typeSuffix,
                         const DogStatsDTags& tags) const noexcept;

  RawChars32 _ns;
  BaseFd _fd;
};

}  // namespace aeronet
