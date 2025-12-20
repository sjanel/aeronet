#pragma once

#include <chrono>
#include <cstdint>

namespace aeronet {

/// HTTP/2 protocol configuration.
///
/// Contains all settings and limits for HTTP/2 connections as defined in RFC 9113.
/// This object is designed to be embedded in HttpServerConfig similar to TLSConfig
/// and CompressionConfig.
///
/// Default values follow RFC 9113 recommendations for a balanced server profile.
struct Http2Config {
  // ============================
  // RFC 9113 SETTINGS parameters
  // ============================

  /// Whether HTTP/2 is enabled, if the client supports it.
  /// Default: true.
  bool enable{true};

  /// SETTINGS_ENABLE_PUSH (0x2): Whether server push is enabled.
  /// Modern clients rarely use push, so it's disabled by default.
  /// Default: false.
  bool enablePush{false};

  /// SETTINGS_HEADER_TABLE_SIZE (0x1): Maximum size of the HPACK dynamic table.
  /// Default: 4096 bytes (RFC 9113 default).
  uint32_t headerTableSize{4096};

  /// SETTINGS_MAX_CONCURRENT_STREAMS (0x3): Maximum concurrent streams per connection.
  /// A reasonable default balancing parallelism and resource usage.
  /// Default: 100.
  uint32_t maxConcurrentStreams{100};

  /// SETTINGS_INITIAL_WINDOW_SIZE (0x4): Initial flow control window size for streams.
  /// Default: 65535 bytes (RFC 9113 default, ~64KB).
  uint32_t initialWindowSize{65535};

  /// SETTINGS_MAX_FRAME_SIZE (0x5): Maximum frame payload size.
  /// Range: 16384 (2^14) to 16777215 (2^24 - 1).
  /// Default: 16384 bytes (RFC 9113 minimum/default).
  uint32_t maxFrameSize{16384};

  /// SETTINGS_MAX_HEADER_LIST_SIZE (0x6): Maximum size of uncompressed header block.
  /// This is an advisory limit. Default: 8192 bytes (reasonable for most use cases).
  uint32_t maxHeaderListSize{8192};

  // ============================
  // Connection-level settings
  // ============================

  /// Initial connection-level flow control window size.
  /// The server will send WINDOW_UPDATE to adjust this upon connection establishment.
  /// Default: 1MB (good for high-throughput scenarios).
  uint32_t connectionWindowSize{1 << 20};

  // ============================
  // Timeouts and limits
  // ============================

  /// Timeout for receiving SETTINGS ACK after sending our SETTINGS frame.
  /// If no ACK is received within this duration, connection is closed with SETTINGS_TIMEOUT.
  /// Default: 5 seconds.
  std::chrono::milliseconds settingsTimeout{std::chrono::milliseconds{5000}};

  /// Timeout for PING/PONG keepalive mechanism.
  /// If enabled (interval > 0), server sends PING frames to detect dead connections.
  /// Default: 0 (disabled).
  std::chrono::milliseconds pingInterval{std::chrono::milliseconds{0}};

  /// Maximum time to wait for PING response before considering connection dead.
  /// Only meaningful when pingInterval > 0.
  /// Default: 10 seconds.
  std::chrono::milliseconds pingTimeout{std::chrono::milliseconds{10000}};

  /// Maximum number of streams that can be created over the lifetime of a connection.
  /// After this limit, the connection will be gracefully closed with GOAWAY.
  /// Default: 0 (unlimited).
  uint32_t maxStreamsPerConnection{0};

  /// Enable cleartext HTTP/2 (h2c) via prior knowledge.
  /// When enabled, the server accepts HTTP/2 connections on non-TLS ports
  /// if the client sends the HTTP/2 connection preface directly.
  /// Default: true (useful for internal microservices).
  bool enableH2c{true};

  /// Enable cleartext HTTP/2 (h2c) via HTTP/1.1 Upgrade mechanism.
  /// Allows clients to upgrade from HTTP/1.1 to HTTP/2 on plaintext connections.
  /// Default: true.
  bool enableH2cUpgrade{true};

  // ============================
  // Priority (RFC 9218 / RFC 9113 §5.3)
  // ============================

  /// Enable HTTP/2 priority handling (PRIORITY frames and stream dependencies).
  /// When disabled, PRIORITY frames are acknowledged but not processed.
  /// Default: true (principle of least surprise).
  bool enablePriority{true};

  /// Maximum depth of the priority dependency tree.
  /// Limits resource usage for malicious deep dependency chains.
  /// Default: 256.
  uint32_t maxPriorityTreeDepth{256};

  // ============================
  // Builder-style setters
  // ============================

  Http2Config& withHeaderTableSize(uint32_t size) {
    headerTableSize = size;
    return *this;
  }

  Http2Config& withEnablePush(bool enable) {
    enablePush = enable;
    return *this;
  }

  Http2Config& withMaxConcurrentStreams(uint32_t maxStreams) {
    maxConcurrentStreams = maxStreams;
    return *this;
  }

  Http2Config& withInitialWindowSize(uint32_t size) {
    initialWindowSize = size;
    return *this;
  }

  Http2Config& withMaxFrameSize(uint32_t size) {
    maxFrameSize = size;
    return *this;
  }

  Http2Config& withMaxHeaderListSize(uint32_t size) {
    maxHeaderListSize = size;
    return *this;
  }

  Http2Config& withConnectionWindowSize(uint32_t size) {
    connectionWindowSize = size;
    return *this;
  }

  Http2Config& withSettingsTimeout(std::chrono::milliseconds timeout) {
    settingsTimeout = timeout;
    return *this;
  }

  Http2Config& withPingInterval(std::chrono::milliseconds interval) {
    pingInterval = interval;
    return *this;
  }

  Http2Config& withPingTimeout(std::chrono::milliseconds timeout) {
    pingTimeout = timeout;
    return *this;
  }

  Http2Config& withMaxStreamsPerConnection(uint32_t maxStreams) {
    maxStreamsPerConnection = maxStreams;
    return *this;
  }

  Http2Config& withEnableH2c(bool enable) {
    enableH2c = enable;
    return *this;
  }

  Http2Config& withEnableH2cUpgrade(bool enable) {
    enableH2cUpgrade = enable;
    return *this;
  }

  Http2Config& withEnablePriority(bool enable) {
    enablePriority = enable;
    return *this;
  }

  Http2Config& withMaxPriorityTreeDepth(uint32_t depth) {
    maxPriorityTreeDepth = depth;
    return *this;
  }

  /// Validates the configuration.
  /// Throws std::invalid_argument if any setting is out of valid range.
  void validate() const;
};

}  // namespace aeronet
