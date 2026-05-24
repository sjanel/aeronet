#pragma once

#include <cstdint>
#include <string>

namespace aeronet {

// Configuration for structured HTTP access logging.
// When sink is None (default), access logging is completely disabled with zero runtime cost.
struct AccessLogConfig {
  void validate() const;

  enum class Sink : uint8_t {
    None,    // Disabled (default) — zero cost
    Stdout,  // Write to stdout
    File     // Append to file at filePath
  };

  enum class Format : uint8_t {
    CLF,  // Combined Log Format (always available)
    JSON  // JSON format (requires AERONET_ENABLE_GLAZE at build time)
  };

  // Output sink. Default: None (disabled).
  Sink sink{Sink::None};

  // Output format. Default: CLF.
  Format format{Format::CLF};

  // When true, use the first IP from X-Forwarded-For header (if present) instead of the direct peer IP.
  // Use this when the server is behind a trusted reverse proxy.
  bool useForwardedFor{false};

  // File path for the access log when sink == File. Ignored otherwise.
  std::string filePath;
};

}  // namespace aeronet
