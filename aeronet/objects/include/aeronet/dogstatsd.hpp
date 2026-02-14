#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "aeronet/dynamic-concatenated-strings.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/unix-socket.hpp"

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
//  - It is not thread-safe.
class DogStatsD {
 private:
  static constexpr const char kCommaSep[] = ",";

 public:
  using DogStatsDTags = DynamicConcatenatedStrings<kCommaSep, uint32_t>;

  // Creates a disabled DogStatsD client.
  DogStatsD() noexcept = default;

  // socketPath: path to unix datagram socket used by the agent (e.g. /var/run/datadog/dsd.socket)
  // ns: optional metric namespace prefix (e.g. "myapp.")
  // Disables the client if socketPath is empty.
  explicit DogStatsD(std::string_view socketPath, std::string_view ns = {});

  void increment(std::string_view metric, uint64_t value = 1UL, const DogStatsDTags& tags = {}) noexcept;

  void gauge(std::string_view metric, int64_t value, const DogStatsDTags& tags = {}) noexcept;

  void histogram(std::string_view metric, double value, const DogStatsDTags& tags = {}) noexcept;

  void timing(std::string_view metric, std::chrono::milliseconds ms, const DogStatsDTags& tags = {}) noexcept;

  void set(std::string_view metric, std::string_view value, const DogStatsDTags& tags = {}) noexcept;

  [[nodiscard]] std::string_view socketPath() const noexcept { return {_buf.data(), _socketPathLength}; }

  [[nodiscard]] std::string_view ns() const noexcept { return {_buf.begin() + _socketPathLength, _buf.end()}; }

  [[nodiscard]] bool enabled() const noexcept { return _socketPathLength != 0; }

 private:
  void sendMetricMessage(std::string_view metric, std::string_view value, std::string_view typeSuffix,
                         const DogStatsDTags& tags) noexcept;

  bool tryReconnect() noexcept;

  [[nodiscard]] bool ensureConnected() noexcept {
    return enabled() && (_retryConnectionCounter == 0 || tryReconnect());
  }

  [[nodiscard]] int connect() noexcept;

  // To avoid trying to reconnect for every message if there is a durable issue
  static constexpr uint8_t kReconnectionThreshold = 50U;  // A very arbitrary number to avoid reconnecting too often

  RawChars32 _buf;
  UnixSocket _fd;
  uint16_t _socketPathLength{0};
  uint8_t _retryConnectionCounter{kReconnectionThreshold};
};

}  // namespace aeronet
