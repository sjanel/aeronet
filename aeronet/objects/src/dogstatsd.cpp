#include "aeronet/dogstatsd.hpp"

#include <cassert>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/decimal-writer.hpp"
#include "aeronet/log-noexcept.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/metric-label.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/system-error-message.hpp"
#include "aeronet/system-error.hpp"
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
  const auto ptr = WriteInt(buffer, value, ndigits(value));
  return {buffer, ptr};
}

}  // namespace

DogStatsD::DogStatsD(std::string_view socketPath, std::string_view ns) {
  if (socketPath.size() >= kUnixSocketMaxPath) {
    throw std::invalid_argument("DogStatsD: socket path too long");
  }
  const uint32_t dotAppendSz = !ns.empty() && ns.back() != '.' ? 1U : 0U;
  if (ns.size() + dotAppendSz > std::numeric_limits<decltype(_nsLength)>::max()) {
    throw std::invalid_argument("DogStatsD: namespace too long");
  }
  if (socketPath.empty()) {
    return;
  }

  _fd = UnixSocket(UnixSocket::Type::Datagram);

  static_assert(kUnixSocketMaxPath <= std::numeric_limits<uint16_t>::max(),
                "uint16_t is not large enough to hold unix socket max path");

  _socketPathLength = static_cast<uint16_t>(socketPath.size());
  _nsLength = static_cast<uint8_t>(ns.size() + dotAppendSz);

  // Room for a full datagram: buffering metrics then never reallocates.
  _buf.reserve(headerSize() + kMaxDatagramSize);
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
    const int serr = LastSystemError();
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

DogStatsD::DogStatsD(DogStatsD&& rhs) noexcept
    : _buf(std::move(rhs._buf)),
      _fd(std::move(rhs._fd)),
      _socketPathLength(std::exchange(rhs._socketPathLength, 0)),
      _nsLength(std::exchange(rhs._nsLength, 0)),
      _retryConnectionCounter(rhs._retryConnectionCounter) {}

DogStatsD& DogStatsD::operator=(DogStatsD&& rhs) noexcept {
  if (this != &rhs) {
    flush();
    _buf = std::move(rhs._buf);
    _fd = std::move(rhs._fd);
    _socketPathLength = std::exchange(rhs._socketPathLength, 0);
    _nsLength = std::exchange(rhs._nsLength, 0);
    _retryConnectionCounter = rhs._retryConnectionCounter;
  }
  return *this;
}

void DogStatsD::appendMetric(std::string_view metric, std::string_view value, std::string_view typeSuffix,
                             const DogStatsDTags& tags, MetricLabels labels) noexcept {
  std::size_t labelsSize = 0;
  for (const auto& [key, labelValue] : labels) {
    labelsSize += key.size() + 1U + labelValue.size();
  }
  if (!labels.empty()) {
    labelsSize += labels.size() - 1U;
  }

  const bool hasTags = !tags.empty() || !labels.empty();
  const auto tagsSize = hasTags ? kTagsPrefix.size() + tags.fullSize() + labelsSize +
                                      static_cast<std::size_t>(!tags.empty() && !labels.empty())
                                : 0UL;
  const auto lineSize = std::size_t{_nsLength} + metric.size() + 1U + value.size() + typeSuffix.size() + tagsSize;

  const std::size_t bufferedSize = _buf.size() - headerSize();
  if (bufferedSize != 0 && bufferedSize + 1U + lineSize > kMaxDatagramSize) {
    flush();
  }
  const bool newLine = _buf.size() != headerSize();

  try {
    _buf.ensureAvailableCapacity(static_cast<std::size_t>(newLine) + lineSize);
  } catch (const std::bad_alloc&) {
    log_noexcept::error("DogStatsD: unable to allocate memory for metric message");
    return;
  }

  char* pData = _buf.data() + _buf.size();

  if (newLine) {
    *pData++ = '\n';
  }
  pData = Append(ns(), pData);
  pData = Append(metric, pData);
  *pData++ = ':';
  pData = Append(value, pData);
  pData = Append(typeSuffix, pData);
  if (hasTags) {
    pData = AppendFixed<kTagsPrefix>(pData);
    if (!tags.empty()) {
      pData = Append(tags.fullString(), pData);
      if (!labels.empty()) {
        *pData++ = ',';
      }
    }
    for (std::size_t idx = 0; idx < labels.size(); ++idx) {
      const auto& [key, labelValue] = labels[idx];
      if (idx != 0) {
        *pData++ = ',';
      }
      pData = Append(key, pData);
      *pData++ = ':';
      pData = Append(labelValue, pData);
    }
  }

  _buf.setEnd(pData);
}

void DogStatsD::flush() noexcept {
  const std::size_t datagramSize = _buf.size() - headerSize();
  if (datagramSize == 0) {
    return;
  }
  if (_fd.send(_buf.data() + headerSize(), datagramSize) == -1) {
    const int serr = LastSystemError();
    // If the socket would block (EAGAIN / EWOULDBLOCK), treat the metrics as dropped
    // and do not mark the connection for immediate reconnect. Other errors indicate
    // a more serious problem with the socket and should trigger a reconnect attempt.
    if (serr == error::kWouldBlock) {
      log_noexcept::debug("DogStatsD: dropping metrics of size {} due to EAGAIN/EWOULDBLOCK", datagramSize);
    } else {
      log_noexcept::error("DogStatsD: unable to send metrics of size {} with error: {}", datagramSize,
                          SystemErrorMessage(serr));
      _retryConnectionCounter = kReconnectionThreshold;  // mark as disconnected but retry immediately on next send
    }
  }
  _buf.setSize(static_cast<RawChars32::size_type>(headerSize()));
}

void DogStatsD::increment(std::string_view metric, uint64_t value, const DogStatsDTags& tags) noexcept {
  if (ensureConnected()) {
    char buffer[kMaxIntegerStrBufferSize];
    appendMetric(metric, FormatInteger(value, buffer), kCounterSuffix, tags, {});
  }
}

void DogStatsD::increment(std::string_view metric, uint64_t value, const DogStatsDTags& tags,
                          MetricLabels labels) noexcept {
  if (ensureConnected()) {
    char buffer[kMaxIntegerStrBufferSize];
    appendMetric(metric, FormatInteger(value, buffer), kCounterSuffix, tags, labels);
  }
}

void DogStatsD::gauge(std::string_view metric, int64_t value, const DogStatsDTags& tags) noexcept {
  if (ensureConnected()) {
    char buffer[kMaxIntegerStrBufferSize];
    appendMetric(metric, FormatInteger(value, buffer), kGaugeSuffix, tags, {});
  }
}

void DogStatsD::gauge(std::string_view metric, int64_t value, const DogStatsDTags& tags, MetricLabels labels) noexcept {
  if (ensureConnected()) {
    char buffer[kMaxIntegerStrBufferSize];
    appendMetric(metric, FormatInteger(value, buffer), kGaugeSuffix, tags, labels);
  }
}

void DogStatsD::histogram(std::string_view metric, double value, const DogStatsDTags& tags) noexcept {
  if (ensureConnected()) {
    char buffer[kFloatingBufferSize];
    appendMetric(metric, FormatFloating(value, buffer), kHistogramSuffix, tags, {});
  }
}

void DogStatsD::histogram(std::string_view metric, double value, const DogStatsDTags& tags,
                          MetricLabels labels) noexcept {
  if (ensureConnected()) {
    char buffer[kFloatingBufferSize];
    appendMetric(metric, FormatFloating(value, buffer), kHistogramSuffix, tags, labels);
  }
}

void DogStatsD::timing(std::string_view metric, std::chrono::milliseconds ms, const DogStatsDTags& tags) noexcept {
  if (ensureConnected()) {
    char buffer[kMaxIntegerStrBufferSize];
    appendMetric(metric, FormatInteger(ms.count(), buffer), kTimingSuffix, tags, {});
  }
}

void DogStatsD::timing(std::string_view metric, std::chrono::milliseconds ms, const DogStatsDTags& tags,
                       MetricLabels labels) noexcept {
  if (ensureConnected()) {
    char buffer[kMaxIntegerStrBufferSize];
    appendMetric(metric, FormatInteger(ms.count(), buffer), kTimingSuffix, tags, labels);
  }
}

void DogStatsD::set(std::string_view metric, std::string_view value, const DogStatsDTags& tags) noexcept {
  if (ensureConnected()) {
    appendMetric(metric, value, kSetSuffix, tags, {});
  }
}

void DogStatsD::set(std::string_view metric, std::string_view value, const DogStatsDTags& tags,
                    MetricLabels labels) noexcept {
  if (ensureConnected()) {
    appendMetric(metric, value, kSetSuffix, tags, labels);
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
  const std::string_view socketPath = this->socketPath();

  assert(_fd);
  _retryConnectionCounter = static_cast<uint8_t>(-_fd.connect(socketPath));
  if (_retryConnectionCounter == 1) {
    log_noexcept::error("DogStatsD: unable to connect to socket '{}'. Full error: {}", socketPath,
                        SystemErrorMessage(LastSystemError()));
  }

  return -_retryConnectionCounter;
}

}  // namespace aeronet
