#include "aeronet/dogstatsd.hpp"

#include <cassert>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

#include "aeronet/log.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/unix-socket.hpp"

#ifndef NDEBUG
#include <system_error>
#endif

namespace aeronet {

namespace {

constexpr std::string_view kTagsPrefix{"|#"};
constexpr std::string_view kCounterSuffix{"|c"};
constexpr std::string_view kGaugeSuffix{"|g"};
constexpr std::string_view kHistogramSuffix{"|h"};
constexpr std::string_view kTimingSuffix{"|ms"};
constexpr std::string_view kSetSuffix{"|s"};

constexpr std::size_t kFloatingBufferSize = std::numeric_limits<double>::max_digits10 + 12;
constexpr std::size_t kMaxIntegerStrBufferSize = std::numeric_limits<uint64_t>::digits10 + 2;  // sign + left-most digit

constexpr std::string_view FormatFloating(double value, char* buffer) {
  const auto [ptr, ec] = std::to_chars(buffer, buffer + kFloatingBufferSize, value, std::chars_format::general);
  assert(ec == std::errc{});
  return {buffer, ptr};
}

constexpr std::string_view FormatInteger(std::integral auto value, char* buffer) {
  const auto [ptr, ec] = std::to_chars(buffer, buffer + kMaxIntegerStrBufferSize, value);
  assert(ec == std::errc{});
  return {buffer, ptr};
}

}  // namespace

DogStatsD::DogStatsD(std::string_view socketPath, std::string_view ns) {
  if (socketPath.size() >= kUnixSocketMaxPath) {
    throw std::invalid_argument("DogStatsD: socket path too long");
  }
  if (ns.size() >= 256UL) {
    throw std::invalid_argument("DogStatsD: namespace too long");
  }
  if (socketPath.empty()) {
    return;
  }

  _fd = UnixSocket(UnixSocket::Type::Datagram);

  static_assert(kUnixSocketMaxPath <= std::numeric_limits<uint16_t>::max(),
                "uint16_t is not large enough to hold unix socket max path");

  _socketPathLength = static_cast<uint16_t>(socketPath.size());

  uint32_t dotAppendSz = !ns.empty() && ns.back() != '.' ? 1U : 0U;

  _buf.reserve(socketPath.size() + ns.size() + dotAppendSz + 64UL);  // additional buffer for metric messages
  _buf.unchecked_append(socketPath);
  _buf.unchecked_append(ns);
  if (dotAppendSz != 0) {
    _buf.unchecked_push_back('.');
  }

  // Perform a single connect attempt to validate the socket path format.
  // Some errors indicate a structural/problematic path (treat as fatal),
  // while others (ENOENT) mean the agent isn't present yet and should be
  // retried later.
  if (connect() == -1) {
    const int serr = errno;
    // Treat these as configuration / syntax / structural errors and throw.
    if (serr == ENOTDIR || serr == EISDIR || serr == ELOOP || serr == EINVAL || serr == ENOTSOCK || serr == EACCES ||
        serr == EPERM) {
      throw std::invalid_argument("DogStatsD: invalid or unusable socket path");
    }
    // Otherwise treat as transient (e.g., ENOENT) and fall back to retry logic.
    // mark as disconnected, but with immediate retry at first message.
    _retryConnectionCounter = kReconnectionThreshold;
  }
}

void DogStatsD::sendMetricMessage(std::string_view metric, std::string_view value, std::string_view typeSuffix,
                                  const DogStatsDTags& tags) noexcept {
  const auto tagsSize = tags.empty() ? 0UL : kTagsPrefix.size() + tags.fullSize();
  const auto nsSize = _buf.size() - _socketPathLength;
  const auto dataSize = nsSize + metric.size() + 1U + value.size() + typeSuffix.size() + tagsSize;

  try {
    _buf.ensureAvailableCapacity(dataSize);
  } catch (const std::bad_alloc&) {
    log::error("DogStatsD: unable to allocate memory for metric message");
    return;
  }

  char* data = _buf.data() + _buf.size();

  data = Append(ns(), data);
  data = Append(metric, data);
  *data++ = ':';
  data = Append(value, data);
  data = Append(typeSuffix, data);
  if (tagsSize != 0) {
    data = Append(kTagsPrefix, data);
    data = Append(tags.fullString(), data);
  }

  if (_fd.send(_buf.data() + _buf.size(), dataSize) == -1) {
    const int serr = errno;
    static_assert(EAGAIN == EWOULDBLOCK, "EAGAIN and EWOULDBLOCK should have the same value");
    // If the socket would block (EAGAIN / EWOULDBLOCK), treat the metric as dropped
    // and do not mark the connection for immediate reconnect. Other errors indicate
    // a more serious problem with the socket and should trigger a reconnect attempt.
    if (serr == EAGAIN) {
      log::debug("DogStatsD: dropping metric of size {} due to EAGAIN/EWOULDBLOCK", dataSize);
    } else {
      log::error("DogStatsD: unable to send message of size {} with error : {}", dataSize, std::strerror(serr));
      _retryConnectionCounter = kReconnectionThreshold;  // mark as disconnected but retry immediately on next send
    }
  }
}

void DogStatsD::increment(std::string_view metric, uint64_t value, const DogStatsDTags& tags) noexcept {
  if (ensureConnected()) {
    char buffer[kMaxIntegerStrBufferSize];
    sendMetricMessage(metric, FormatInteger(value, buffer), kCounterSuffix, tags);
  }
}

void DogStatsD::gauge(std::string_view metric, int64_t value, const DogStatsDTags& tags) noexcept {
  if (ensureConnected()) {
    char buffer[kMaxIntegerStrBufferSize];
    sendMetricMessage(metric, FormatInteger(value, buffer), kGaugeSuffix, tags);
  }
}

void DogStatsD::histogram(std::string_view metric, double value, const DogStatsDTags& tags) noexcept {
  if (ensureConnected()) {
    char buffer[kFloatingBufferSize];
    sendMetricMessage(metric, FormatFloating(value, buffer), kHistogramSuffix, tags);
  }
}

void DogStatsD::timing(std::string_view metric, std::chrono::milliseconds ms, const DogStatsDTags& tags) noexcept {
  if (ensureConnected()) {
    char buffer[kMaxIntegerStrBufferSize];
    sendMetricMessage(metric, FormatInteger(ms.count(), buffer), kTimingSuffix, tags);
  }
}

void DogStatsD::set(std::string_view metric, std::string_view value, const DogStatsDTags& tags) noexcept {
  if (ensureConnected()) {
    sendMetricMessage(metric, value, kSetSuffix, tags);
  }
}

bool DogStatsD::tryReconnect() noexcept {
  assert(_retryConnectionCounter != 0);
  if (++_retryConnectionCounter < kReconnectionThreshold) {
    return false;
  }
  return connect() == 0;
}

int DogStatsD::connect() noexcept {
  std::string_view socketPath = this->socketPath();

  assert(_fd);

  const int ret = _fd.connect(socketPath);
  if (ret == -1) {
    log::error("DogStatsD: unable to connect to socket '{}'. Full error: {}", socketPath, std::strerror(errno));
    _retryConnectionCounter = 1;
  } else {
    _retryConnectionCounter = 0;
  }

  return ret;
}

}  // namespace aeronet
