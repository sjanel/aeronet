#pragma once

#include "aeronet/access-log-config.hpp"
#include "aeronet/base-fd.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/request-metrics.hpp"

namespace aeronet {

// Buffered access log writer that formats and emits one log line per request.
// Thread model: one instance per event-loop thread; no internal locking.
// When sink is None, construction is a no-op and log() returns immediately.
class AccessLogWriter {
 public:
  // Constructs a disabled AccessLogWriter when config.sink == None, otherwise initializes according to config.
  AccessLogWriter() noexcept = default;

  explicit AccessLogWriter(const AccessLogConfig& config);

  AccessLogWriter(const AccessLogWriter&) = delete;
  AccessLogWriter& operator=(const AccessLogWriter&) = delete;

  AccessLogWriter(AccessLogWriter&&) noexcept = default;
  AccessLogWriter& operator=(AccessLogWriter&&) noexcept = default;

  ~AccessLogWriter() { flush(); }

  [[nodiscard]] operator bool() const noexcept { return _sink != AccessLogConfig::Sink::None; }

  // Format and buffer a log line for the given request metrics.
  void log(const RequestMetrics& metrics);

  // Flush any buffered log data to the configured sink.
  void flush();

 private:
  void formatCLF(const RequestMetrics& metrics);
  void formatJSON(const RequestMetrics& metrics);

  RawChars32 _buffer;
  BaseFd _fileFd;  // Valid if sink == File, ignored otherwise
  AccessLogConfig::Format _format = AccessLogConfig::Format::CLF;
  AccessLogConfig::Sink _sink = AccessLogConfig::Sink::None;
};

}  // namespace aeronet
