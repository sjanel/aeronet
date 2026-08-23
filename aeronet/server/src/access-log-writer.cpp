#include "aeronet/access-log-writer.hpp"

#include <fcntl.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string_view>

#ifdef AERONET_WINDOWS
#include <io.h>
#include <sys/stat.h>

#include "aeronet/safe-cast.hpp"
#else
#include <unistd.h>
#endif

#include "aeronet/access-log-config.hpp"
#include "aeronet/decimal-writer.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/log-noexcept.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/request-metrics.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet {

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

  if (_format == AccessLogConfig::Format::CLF) {
    formatCLF(metrics);
  } else {
    assert(_format == AccessLogConfig::Format::JSON);
    formatJSON(metrics);
  }

  static constexpr decltype(_buffer)::size_type kFlushThreshold = 8192;

  if (_buffer.size() >= kFlushThreshold) {
    flush();
  }
}

void AccessLogWriter::formatCLF(const RequestMetrics& metrics) {
  // CLF Combined format:
  // <ip> - - [<timestamp>] "<method> <path> HTTP/<ver>" <status> <bytesOut> "-" "<ua>"
  // Worst case estimate: ip + 26(ts) + 10(method) + path + 8(ver) + 6(status) + 20(bytes) + ua + overhead

  static constexpr std::string_view kSep1 = " - - [";
  static constexpr std::string_view kReferer = R"( "-" ")";

  const auto methodStr = http::MethodToStr(metrics.method);
  const auto nDigitsMetricsBytesOut = ndigits(metrics.bytesOut);

  _buffer.ensureAvailableCapacityExponential(metrics.clientIp.size() + kSep1.size() + ISO8601UTCWithMsStrLen + 3U +
                                             methodStr.size() + 1U + metrics.path.size() + 1U + 8U + 2U + 3U + 1U +
                                             nDigitsMetricsBytesOut + kReferer.size() + metrics.userAgent.size() + 2U);

  char* pData = _buffer.data() + _buffer.size();

  // Client IP
  pData = Append(metrics.clientIp, pData);

  // " - - ["
  pData = AppendFixed<kSep1>(pData);

  // Timestamp in ISO 8601 with ms
  pData = TimeToStringISO8601UTCWithMs(SysClock::now(), pData);

  // "] \""
  static constexpr std::string_view kTsEnd = "] \"";
  pData = AppendFixed<kTsEnd>(pData);

  // Method
  pData = Append(methodStr, pData);
  *pData++ = ' ';

  // Path
  pData = Append(metrics.path, pData);
  *pData++ = ' ';

  // "HTTP/X.Y"
  pData = metrics.version.writeFull(pData);

  // "\" <status> <bytesOut>"
  *pData++ = '"';
  *pData++ = ' ';
  pData = writeStatusCode(pData, metrics.status);
  *pData++ = ' ';
  pData = WriteInt(pData, metrics.bytesOut, nDigitsMetricsBytesOut);

  // " \"-\" \""
  pData = AppendFixed<kReferer>(pData);

  // User-Agent
  if (!metrics.userAgent.empty()) {
    pData = Append(metrics.userAgent, pData);
  }

  // "\"\n"
  *pData++ = '"';
  *pData++ = '\n';

  _buffer.setEnd(pData);
}

void AccessLogWriter::formatJSON(const RequestMetrics& metrics) {
  auto methodStr = http::MethodToStr(metrics.method);
  auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(metrics.duration).count();

  static constexpr std::string_view kTsPart = R"({"ts":")";
  static constexpr std::string_view kMethodPart = R"(","method":")";
  static constexpr std::string_view kPathPart = R"(","path":")";
  static constexpr std::string_view kStatusPart = R"(","status":)";
  static constexpr std::string_view kBytesOutPart = R"(,"bytesOut":)";
  static constexpr std::string_view kDurationPart = R"(,"durationUs":)";
  static constexpr std::string_view kIpPart = R"(,"ip":")";
  static constexpr std::string_view kUaPart = R"(","ua":")";
  static constexpr std::string_view kEndPart = "\"}\n";

  const auto nDigitsMetricsBytesOut = ndigits(metrics.bytesOut);
  const auto nDigitsDurationUs = ndigits(durationUs);

  _buffer.ensureAvailableCapacityExponential(
      kTsPart.size() + ISO8601UTCWithMsStrLen + kMethodPart.size() + methodStr.size() + kPathPart.size() +
      metrics.path.size() + kStatusPart.size() + 3U + kBytesOutPart.size() + nDigitsMetricsBytesOut +
      kDurationPart.size() + nDigitsDurationUs + kIpPart.size() + metrics.clientIp.size() + kUaPart.size() +
      metrics.userAgent.size() + kEndPart.size());

  // Manual JSON to avoid Glaze linkage issues with local/anonymous types.
  char* out = _buffer.data() + _buffer.size();

  out = AppendFixed<kTsPart>(out);
  out = TimeToStringISO8601UTCWithMs(SysClock::now(), out);
  out = AppendFixed<kMethodPart>(out);
  out = Append(methodStr, out);
  out = AppendFixed<kPathPart>(out);
  out = Append(metrics.path, out);
  out = AppendFixed<kStatusPart>(out);
  out = writeStatusCode(out, metrics.status);
  out = AppendFixed<kBytesOutPart>(out);
  out = WriteInt(out, metrics.bytesOut, nDigitsMetricsBytesOut);
  out = AppendFixed<kDurationPart>(out);
  out = WriteInt(out, durationUs, nDigitsDurationUs);
  out = AppendFixed<kIpPart>(out);
  out = Append(metrics.clientIp, out);
  out = AppendFixed<kUaPart>(out);
  out = Append(metrics.userAgent, out);
  out = AppendFixed<kEndPart>(out);

  _buffer.setSize(static_cast<decltype(_buffer)::size_type>(out - _buffer.data()));
}

void AccessLogWriter::flush() noexcept {
  int fd;
  if (_sink == AccessLogConfig::Sink::File) {
    fd = static_cast<int>(_fileFd.fd());
  } else {
    fd = 1;
  }

  const char* const pStart = _buffer.data();
  const char* pData = pStart;

  for (std::size_t remaining = _buffer.size(); remaining != 0;) {
#ifdef AERONET_WINDOWS
    const auto chunk = SafeCast<unsigned int>(remaining);
    const auto written = ::_write(fd, pData, chunk);
#else
    const auto written = ::write(fd, pData, remaining);
#endif
    if (written <= 0) {
      // flush is noexcept, so logging failures must not escape this path.
      log_noexcept::error("access log write failed on fd {}: errno {}", fd, errno);
      _buffer.clear();
      _sink = AccessLogConfig::Sink::None;  // Disable further logging on error
      return;
    }

    pData += written;
    remaining -= static_cast<std::size_t>(written);
  }

  assert(static_cast<decltype(_buffer.size())>(pData - pStart) == _buffer.size());
  _buffer.clear();
}

}  // namespace aeronet
