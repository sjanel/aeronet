#include "aeronet/access-log-writer.hpp"

#include <cassert>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "aeronet/time-constants.hpp"
#ifndef NDEBUG
#include <system_error>
#endif

#ifdef AERONET_WINDOWS
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "aeronet/http-method.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/request-metrics.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet {

namespace {

constexpr char* AppendIntegral(char* out, std::integral auto value) {
  const auto [ptr, ec] = std::to_chars(out, out + std::numeric_limits<decltype(value)>::digits10 + 1, value);
  assert(ec == std::errc{});
  return ptr;
}

}  // namespace

AccessLogWriter::AccessLogWriter(const AccessLogConfig& config) : _format(config.format), _sink(config.sink) {
  if (_sink == AccessLogConfig::Sink::None) {
    return;
  }

  config.validate();

  if (_sink == AccessLogConfig::Sink::File) {
#ifdef AERONET_WINDOWS
    const int fd = ::_open(config.filePath.c_str(), O_WRONLY | O_CREAT | O_APPEND, S_IREAD | S_IWRITE);
#else
    const int fd = ::open(config.filePath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
#endif
    if (fd < 0) {
      log::error("Failed to open access log file '{}'", config.filePath);
      throw std::runtime_error("Failed to open access log file");
    }
#ifdef AERONET_WINDOWS
    _fileFd = BaseFd(static_cast<NativeHandle>(fd), BaseFd::HandleKind::CrtFd);
#else
    _fileFd = BaseFd(fd);
#endif
  }
}

void AccessLogWriter::log(const RequestMetrics& metrics) {
  assert(_sink != AccessLogConfig::Sink::None);

  switch (_format) {
    case AccessLogConfig::Format::CLF:
      formatCLF(metrics);
      break;
    default:
      assert(_format == AccessLogConfig::Format::JSON);
      formatJSON(metrics);
      break;
  }

  static constexpr RawChars32::size_type kFlushThreshold = 8192;

  if (_buffer.size() >= kFlushThreshold) {
    flush();
  }
}

void AccessLogWriter::formatCLF(const RequestMetrics& metrics) {
  // CLF Combined format:
  // <ip> - - [<timestamp>] "<method> <path> HTTP/<ver>" <status> <bytesOut> "-" "<ua>"
  // Worst case estimate: 46(ip) + 26(ts) + 10(method) + path + 8(ver) + 6(status) + 20(bytes) + ua + overhead

  static constexpr std::string_view kSep1 = " - - [";
  static constexpr std::string_view kReferer = R"( "-" ")";

  const auto methodStr = http::MethodToStr(metrics.method);

  _buffer.ensureAvailableCapacityExponential(metrics.clientIp.size() + kSep1.size() + ISO8601UTCWithMsStrLen + 3U +
                                             methodStr.size() + 1U + metrics.path.size() + 1U + 8U + 2U + 3U + 1U +
                                             (std::numeric_limits<decltype(metrics.bytesOut)>::digits10 + 1U) +
                                             kReferer.size() + metrics.userAgent.size() + 2U);

  char* out = _buffer.data() + _buffer.size();

  // Client IP
  out = Append(metrics.clientIp, out);

  // " - - ["
  out = Append(kSep1, out);

  // Timestamp in ISO 8601 with ms
  out = TimeToStringISO8601UTCWithMs(SysClock::now(), out);

  // "] \""
  *out++ = ']';
  *out++ = ' ';
  *out++ = '"';

  // Method
  out = Append(methodStr, out);
  *out++ = ' ';

  // Path
  out = Append(metrics.path, out);
  *out++ = ' ';

  // "HTTP/X.Y"
  out = metrics.version.writeFull(out);

  // "\" <status> <bytesOut>"
  *out++ = '"';
  *out++ = ' ';
  out = write3(out, metrics.status);
  *out++ = ' ';
  out = AppendIntegral(out, metrics.bytesOut);

  // " \"-\" \""
  out = Append(kReferer, out);

  // User-Agent
  if (!metrics.userAgent.empty()) {
    out = Append(metrics.userAgent, out);
  }

  // "\"\n"
  *out++ = '"';
  *out++ = '\n';

  _buffer.setSize(static_cast<uint32_t>(out - _buffer.data()));
}

void AccessLogWriter::formatJSON(const RequestMetrics& metrics) {
  auto methodStr = http::MethodToStr(metrics.method);
  auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(metrics.duration).count();

  static constexpr uint32_t kMaxCharsDuration = std::numeric_limits<decltype(durationUs)>::digits10 + 1;

  static constexpr std::string_view kTsPart = R"({"ts":")";
  static constexpr std::string_view kMethodPart = R"(","method":")";
  static constexpr std::string_view kPathPart = R"(","path":")";
  static constexpr std::string_view kStatusPart = R"(","status":)";
  static constexpr std::string_view kBytesOutPart = R"(,"bytesOut":)";
  static constexpr std::string_view kDurationPart = R"(,"durationUs":)";
  static constexpr std::string_view kIpPart = R"(,"ip":")";
  static constexpr std::string_view kUaPart = R"(","ua":")";
  static constexpr std::string_view kEndPart = "\"}\n";

  _buffer.ensureAvailableCapacityExponential(
      kTsPart.size() + ISO8601UTCWithMsStrLen + kMethodPart.size() + methodStr.size() + kPathPart.size() +
      metrics.path.size() + kStatusPart.size() + 3U + kBytesOutPart.size() +
      (std::numeric_limits<decltype(metrics.bytesOut)>::digits10 + 1U) + kDurationPart.size() + kMaxCharsDuration +
      kIpPart.size() + metrics.clientIp.size() + kUaPart.size() + metrics.userAgent.size() + kEndPart.size());

  // Manual JSON to avoid Glaze linkage issues with local/anonymous types.
  char* out = _buffer.data() + _buffer.size();

  out = Append(kTsPart, out);
  out = TimeToStringISO8601UTCWithMs(SysClock::now(), out);
  out = Append(kMethodPart, out);
  out = Append(methodStr, out);
  out = Append(kPathPart, out);
  out = Append(metrics.path, out);
  out = Append(kStatusPart, out);
  out = write3(out, metrics.status);
  out = Append(kBytesOutPart, out);
  out = AppendIntegral(out, metrics.bytesOut);
  out = Append(kDurationPart, out);
  out = AppendIntegral(out, durationUs);
  out = Append(kIpPart, out);
  out = Append(metrics.clientIp, out);
  out = Append(kUaPart, out);
  out = Append(metrics.userAgent, out);
  out = Append(kEndPart, out);

  _buffer.setSize(static_cast<uint32_t>(out - _buffer.data()));
}

void AccessLogWriter::flush() {
  if (_buffer.size() == 0) {
    return;
  }

  int fd;
  switch (_sink) {
    case AccessLogConfig::Sink::Stdout:
      fd = 1;
      break;
    default:
      assert(_sink == AccessLogConfig::Sink::File);
      fd = static_cast<int>(_fileFd.fd());
      break;
  }

  const char* const start = _buffer.data();
  const char* data = start;
  std::size_t remaining = _buffer.size();

  do {
#ifdef AERONET_WINDOWS
    const auto chunk = remaining > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())
                           ? std::numeric_limits<unsigned int>::max()
                           : static_cast<unsigned int>(remaining);
    const auto written = ::_write(fd, data, chunk);
#else
    const auto written = ::write(fd, data, remaining);
#endif
    if (written <= 0) {
      log::error("access log write failed on fd {}: errno {}", fd, errno);
      _buffer.clear();
      _sink = AccessLogConfig::Sink::None;  // Disable further logging on error
      return;
    }

    data += written;
    remaining -= static_cast<std::size_t>(written);
  } while (remaining > 0);

  assert(data > start);
  _buffer.erase_front(static_cast<uint32_t>(data - start));
}

}  // namespace aeronet
