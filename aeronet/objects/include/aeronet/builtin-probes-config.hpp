#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "aeronet/static-concatenated-strings.hpp"

namespace aeronet {

class BuiltinProbesConfig {
 public:
  void validate() const;

  [[nodiscard]] std::string_view livenessPath() const noexcept { return _paths[0]; }

  [[nodiscard]] std::string_view readinessPath() const noexcept { return _paths[1]; }

  [[nodiscard]] std::string_view startupPath() const noexcept { return _paths[2]; }

  BuiltinProbesConfig& withLivenessPath(std::string_view path) {
    _paths.set(0, path);
    return *this;
  }

  BuiltinProbesConfig& withReadinessPath(std::string_view path) {
    _paths.set(1, path);
    return *this;
  }

  BuiltinProbesConfig& withStartupPath(std::string_view path) {
    _paths.set(2, path);
    return *this;
  }

  // Configure a dedicated probe listener served by its own thread on the given port (see dedicatedPort).
  BuiltinProbesConfig& withDedicatedPort(std::uint16_t port) {
    dedicatedPort = port;
    return *this;
  }

  // Set the liveness staleness threshold used by the dedicated probe listener (see livenessStaleThreshold).
  BuiltinProbesConfig& withLivenessStaleThreshold(std::chrono::milliseconds threshold) {
    livenessStaleThreshold = threshold;
    return *this;
  }

  // We may add more content types in the future.
  enum class ContentType : std::uint8_t { TextPlainUtf8 };

  // Liveness staleness threshold used by the dedicated probe listener's heartbeat (see dedicatedPort).
  // The pod is reported live unless EVERY worker event loop has been blocked inside a request handler for
  // longer than this duration. Set it above your longest legitimate handler runtime, especially when the
  // server runs with only a few threads. Only used when dedicatedPort != 0. Must be strictly positive then.
  std::chrono::milliseconds livenessStaleThreshold{std::chrono::seconds{10}};

  // TCP port for a dedicated probe listener, served by its own single-threaded event loop (MultiHttpServer only).
  // 0 (default) => probes are served inline on the main application port, sharing the worker event loops and thus
  // subject to head-of-line blocking when a worker is busy in a long request handler.
  // When non-zero, MultiHttpServer starts an extra thread whose sole job is answering the probe endpoints on this
  // port, so probe availability is isolated from application load. Point your Kubernetes probes at this port.
  // Ignored by a standalone SingleHttpServer (it has no worker pool to isolate probes from).
  std::uint16_t dedicatedPort{0};

  bool enabled{false};

  // Unused for now.
  ContentType contentType{ContentType::TextPlainUtf8};

 private:
  using Paths = StaticConcatenatedStrings<3, uint32_t>;

  Paths _paths{"/livez", "/readyz", "/startupz"};
};

}  // namespace aeronet