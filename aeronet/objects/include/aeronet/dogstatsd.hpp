#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/dynamic-concatenated-strings.hpp"
#include "aeronet/metric-label.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/unix-socket.hpp"

namespace aeronet {

// Lightweight DogStatsD client
// Sends Datadog-compatible StatsD messages over a Unix domain datagram socket (UDS).
// Usage:
//   #include "dogstatsd.hpp"
//   DogStatsD statsd("/var/run/datadog/dsd.socket", "myapp");
//   statsd.increment("requests.processed", 1, {"route:home", "env:prod"});
//   statsd.flush();
// Notes:
//  - Uses SOCK_DGRAM AF_UNIX; requires Datadog Agent configured to listen on that socket.
//  - Metrics are buffered, and sent several per datagram (one per line): a datagram is sent when the next metric would
//    make it larger than kMaxDatagramSize, at flush() and at destruction.
//  - If socket creation/send fails, construction of the object throws.
//  - This is intentionally minimal and dependency-free; it focuses on non-blocking sends and simple
//    formatting (tags as dogstatsd format: |#tag1,tag2:value).
//  - It is not thread-safe.
class DogStatsD {
 private:
  static constexpr const char kCommaSep[] = ",";

 public:
  using DogStatsDTags = DynamicConcatenatedStrings<kCommaSep, uint32_t>;

  // Maximum size of a datagram of several metrics: the default receive buffer size of the Datadog Agent
  // (dogstatsd_buffer_size). A single metric larger than that is sent alone.
  static constexpr std::size_t kMaxDatagramSize = 8192;

  // Creates a disabled DogStatsD client.
  DogStatsD() noexcept = default;

  // socketPath: path to unix datagram socket used by the agent (e.g. /var/run/datadog/dsd.socket)
  // ns: optional metric namespace prefix (e.g. "myapp."), of at most 255 characters with its trailing dot.
  // Disables the client if socketPath is empty.
  explicit DogStatsD(std::string_view socketPath, std::string_view ns = {});

  DogStatsD(const DogStatsD&) = delete;
  DogStatsD(DogStatsD&& rhs) noexcept;
  DogStatsD& operator=(const DogStatsD&) = delete;
  // Flushes the metrics buffered by this client before taking over those of rhs.
  DogStatsD& operator=(DogStatsD&& rhs) noexcept;

  // Sends the buffered metrics.
  ~DogStatsD() { flush(); }

  void increment(std::string_view metric, uint64_t value = 1UL, const DogStatsDTags& tags = {}) noexcept;

  void increment(std::string_view metric, uint64_t value, const DogStatsDTags& tags, MetricLabels labels) noexcept;

  void gauge(std::string_view metric, int64_t value, const DogStatsDTags& tags = {}) noexcept;

  void gauge(std::string_view metric, int64_t value, const DogStatsDTags& tags, MetricLabels labels) noexcept;

  void histogram(std::string_view metric, double value, const DogStatsDTags& tags = {}) noexcept;

  void histogram(std::string_view metric, double value, const DogStatsDTags& tags, MetricLabels labels) noexcept;

  void timing(std::string_view metric, std::chrono::milliseconds ms, const DogStatsDTags& tags = {}) noexcept;

  void timing(std::string_view metric, std::chrono::milliseconds ms, const DogStatsDTags& tags,
              MetricLabels labels) noexcept;

  void set(std::string_view metric, std::string_view value, const DogStatsDTags& tags = {}) noexcept;

  void set(std::string_view metric, std::string_view value, const DogStatsDTags& tags, MetricLabels labels) noexcept;

  // Sends the buffered metrics, if any.
  void flush() noexcept;

  [[nodiscard]] std::string_view socketPath() const noexcept { return {_buf.data(), _socketPathLength}; }

  [[nodiscard]] std::string_view ns() const noexcept { return {_buf.data() + _socketPathLength, _nsLength}; }

  [[nodiscard]] bool enabled() const noexcept { return _socketPathLength != 0; }

 private:
  void appendMetric(std::string_view metric, std::string_view value, std::string_view typeSuffix,
                    const DogStatsDTags& tags, MetricLabels labels) noexcept;

  // Size of the socket path and of the namespace, stored before the buffered metrics.
  [[nodiscard]] std::size_t headerSize() const noexcept { return std::size_t{_socketPathLength} + _nsLength; }

  bool tryReconnect() noexcept;

  [[nodiscard]] bool ensureConnected() noexcept {
    return enabled() && (_retryConnectionCounter == 0 || tryReconnect());
  }

  [[nodiscard]] int connect() noexcept;

  // The socket path, the namespace, then the buffered metrics (newline separated).
  RawChars32 _buf;
  UnixSocket _fd;
  uint16_t _socketPathLength{0};
  uint8_t _nsLength{0};
  uint8_t _retryConnectionCounter{kReconnectionThreshold};

  // To avoid trying to reconnect for every message if there is a durable issue
  // A very arbitrary number to avoid reconnecting too often
  static constexpr decltype(_retryConnectionCounter) kReconnectionThreshold = 50U;
};

}  // namespace aeronet
