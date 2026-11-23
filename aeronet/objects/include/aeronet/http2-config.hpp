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
  /// Validates the configuration.
  /// Throws std::invalid_argument if any setting is out of valid range.
  void validate() const;

  // ============================
  // Builder-style setters
  // ============================

  Http2Config& withHeaderTableSize(uint32_t size) {
    headerTableSize = size;
    return *this;
  }

  /// Deprecated: no effect, see enablePush.
  Http2Config& withEnablePush(bool on) {
    enablePush = on;
    return *this;
  }

  Http2Config& withMergeUnknownRequestHeaders(bool on) {
    this->mergeUnknownRequestHeaders = on;
    return *this;
  }

  Http2Config& withSendContentLengthHeader(bool on) {
    sendContentLengthHeader = on;
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

  /// See settingsTimeout. std::chrono::milliseconds::max() means no timeout.
  Http2Config& withSettingsTimeout(std::chrono::milliseconds timeout) {
    settingsTimeout = timeout;
    return *this;
  }

  /// See pingInterval. std::chrono::milliseconds::max() means no PING.
  Http2Config& withPingInterval(std::chrono::milliseconds interval) {
    pingInterval = interval;
    return *this;
  }

  /// See pingTimeout. std::chrono::milliseconds::max() means no timeout.
  Http2Config& withPingTimeout(std::chrono::milliseconds timeout) {
    pingTimeout = timeout;
    return *this;
  }

  /// See streamIdleTimeout. std::chrono::milliseconds::max() means no timeout.
  Http2Config& withStreamIdleTimeout(std::chrono::milliseconds timeout) {
    streamIdleTimeout = timeout;
    return *this;
  }

  Http2Config& withMaxStreamsPerConnection(uint32_t maxStreams) {
    maxStreamsPerConnection = maxStreams;
    return *this;
  }

  /// See maxResetStreams. std::numeric_limits<uint32_t>::max() means no limit.
  Http2Config& withMaxResetStreams(uint32_t maxResets) {
    maxResetStreams = maxResets;
    return *this;
  }

  Http2Config& withMaxStreamPendingBytes(uint32_t maxPending) {
    maxStreamPendingBytes = maxPending;
    return *this;
  }

  Http2Config& withEnableH2c(bool on) {
    enableH2c = on;
    return *this;
  }

  /// Deprecated: no effect, see enableH2cUpgrade.
  Http2Config& withEnableH2cUpgrade(bool on) {
    enableH2cUpgrade = on;
    return *this;
  }

  Http2Config& withEnablePriority(bool on) {
    enablePriority = on;
    return *this;
  }

  Http2Config& withMaxPriorityTreeDepth(uint16_t depth) {
    maxPriorityTreeDepth = depth;
    return *this;
  }

  Http2Config& withMaxRecycledRequests(uint8_t maxRecycled) {
    maxRecycledRequests = maxRecycled;
    return *this;
  }

  /// See indexRequestPath.
  Http2Config& withIndexRequestPath(bool on) {
    indexRequestPath = on;
    return *this;
  }

  // ============================
  // RFC 9113 SETTINGS parameters
  // ============================

  /// Whether HTTP/2 is enabled, if the client supports it.
  /// Default: true.
  bool enable{true};

  /// Deprecated, ignored: server push is not supported. SETTINGS_ENABLE_PUSH (0x2) is always advertised as 0, by
  /// servers (RFC 9113 §6.5.2 forbids them to send 1, clients reject it with a connection error) and by clients.
  /// Kept so that existing code and configuration files still compile and load.
  /// Default: false.
  bool enablePush{false};

  /// Enable cleartext HTTP/2 (h2c) via prior knowledge.
  /// When enabled, the server accepts HTTP/2 connections on non-TLS ports if the client sends the HTTP/2 connection
  /// preface directly.
  // Default: true (useful for internal microservices).
  bool enableH2c{true};

  /// Deprecated, ignored: the HTTP/1.1 `Upgrade: h2c` mechanism is not supported (RFC 9113 §3.1 deprecated it).
  /// Such requests are answered over HTTP/1.1 whatever this value is; clients wanting cleartext HTTP/2 must use
  /// prior knowledge (see enableH2c). Kept so that existing code and configuration files still compile and load.
  /// Default: false.
  bool enableH2cUpgrade{false};

  /// Whether to record on the streams the priorities of the peer (PRIORITY frames and priority fields of HEADERS
  /// frames), within maxPriorityTreeDepth. They do not change the order of the responses: when disabled, these frames
  /// are still validated, only this bookkeeping is skipped.
  /// Default: true.
  bool enablePriority{true};

  /// Whether to merge multiple header values for unknown request headers.
  /// If true, multiple values for unknown headers are concatenated with commas, per RFC 9113 §8.1.2. If false, a
  /// repeated unknown header is rejected. Default: true.
  bool mergeUnknownRequestHeaders{true};

  /// Whether to automatically add a Content-Length header to HTTP/2 responses built via
  /// HttpRequestView::makeResponse(). Content-Length is optional in HTTP/2, but sending it can improve interoperability
  /// with some clients and intermediaries.
  ///
  /// Only applies to responses produced by makeResponse(); manually constructed HttpResponse objects are unaffected.
  /// Not yet honored for outgoing HttpClient requests.
  ///
  /// Disabling this can be a worthwhile optimization when headers and body share the same buffer: it avoids relocating
  /// the body every time the content length's digit count changes (e.g. during automatic compression or incremental
  /// body appends).
  ///
  /// Default: true.
  bool sendContentLengthHeader{true};

  /// Server only: number of request objects a connection keeps from its finished streams for its next ones, which reuse
  /// them along with the capacity of their header maps and buffers (up to 16 KiB each) instead of allocating them. A
  /// request is released as soon as it is answered, so more are only needed by connections receiving the bodies of
  /// that many requests (or running that many suspended async handlers) at the same time; each one kept costs some
  /// memory to the connection while idle. 0 disables the recycling.
  /// Default: 8.
  uint8_t maxRecycledRequests{8};

  /// SETTINGS_HEADER_TABLE_SIZE (0x1): Maximum size of the HPACK dynamic table of the decoder, enforced on the size
  /// updates of the peer. Also caps the table of the encoder, whatever the peer advertises.
  /// Default: 4096 bytes (RFC 9113 default).
  uint32_t headerTableSize{4096};

  /// SETTINGS_MAX_CONCURRENT_STREAMS (0x3): Maximum concurrent streams per connection.
  /// A reasonable default balancing parallelism and resource usage.
  /// A stream closed while the deferred work of its async handler runs keeps counting until the work completes.
  /// Default: 100.
  uint32_t maxConcurrentStreams{100};

  /// SETTINGS_INITIAL_WINDOW_SIZE (0x4): Initial flow control window size for streams.
  /// The peer sends at most this many bytes on a stream before receiving credit: with the RFC 9113 default (65535
  /// bytes), a single upload (server) or download (client) would be capped to about 64 KiB per round trip.
  /// Must be at least 1. A value below 65535 applies once the peer has acknowledged it: until then, the peer may use
  /// the default window on the streams it opens.
  /// Default: 1 MiB.
  uint32_t initialWindowSize{1U << 20U};

  /// SETTINGS_MAX_FRAME_SIZE (0x5): Maximum frame payload size.
  /// Range: 16384 (2^14) to 16777215 (2^24 - 1).
  /// Default: 16384 bytes (RFC 9113 minimum/default).
  uint32_t maxFrameSize{16384};

  /// SETTINGS_MAX_HEADER_LIST_SIZE (0x6): Maximum decoded header-list size this endpoint accepts.
  /// Each field contributes its name, value, and 32 bytes of overhead. This is advertised as an advisory limit.
  /// Default: 8192 bytes (reasonable for most use cases).
  uint32_t maxHeaderListSize{8192};

  // ============================
  // Connection-level settings
  // ============================

  /// Initial connection-level flow control window size.
  /// The server will send WINDOW_UPDATE to adjust this upon connection establishment.
  /// Must be at least 65535: the window starts there on every connection, and no frame can reduce it.
  /// Default: 1MB (good for high-throughput scenarios).
  uint32_t connectionWindowSize{1U << 20U};

  // ============================
  // Timeouts and limits
  // ============================

  /// Server only: time the client has to acknowledge the SETTINGS of the server, after which the connection is closed
  /// with SETTINGS_TIMEOUT (RFC 9113 §6.5.3). Checked by the periodic maintenance, which starts the wait at its first
  /// pass after the SETTINGS were sent (at most HttpServerConfig::pollInterval later).
  /// Must be positive. std::chrono::milliseconds::max() means no timeout.
  /// Default: 5 seconds.
  std::chrono::milliseconds settingsTimeout{std::chrono::seconds{5}};

  /// Server only: time without any activity on a connection whose streams are open after which the server sends it a
  /// PING, to detect a client that disappeared (see pingTimeout). Such a connection is never idle:
  /// HttpServerConfig::keepAliveTimeout does not apply to it. A connection without open stream is not pinged.
  /// Must be positive. std::chrono::milliseconds::max() means no PING.
  /// Default: 30 seconds.
  std::chrono::milliseconds pingInterval{std::chrono::seconds{30}};

  /// Server only: time the client has to acknowledge a PING sent after pingInterval, after which the connection is
  /// closed.
  /// Must be positive. std::chrono::milliseconds::max() means no timeout.
  /// Default: 10 seconds.
  std::chrono::milliseconds pingTimeout{std::chrono::seconds{10}};

  /// Server only: time a stream may wait for the client without progress. A request whose body stalls that long is
  /// answered 408 Request Timeout, then its stream is reset with NO_ERROR; a response waiting that long for
  /// flow-control credit, or for the client to read it, is reset with CANCEL. Each DATA frame received with data
  /// restarts the wait of a request body, each DATA frame sent restarts the wait of a response: the frames of other
  /// streams, or PING frames, do not. A connection with open streams is never idle (HttpServerConfig::keepAliveTimeout
  /// does not apply to it): this bounds the time a client keeps the memory of its streams without using it. CONNECT
  /// tunnels and streams whose handler runs are not concerned.
  /// Must be positive. std::chrono::milliseconds::max() means no timeout.
  /// Default: 60 seconds.
  std::chrono::milliseconds streamIdleTimeout{std::chrono::seconds{60}};

  /// Maximum response payload bytes retained for one stream while peer flow-control windows are exhausted.
  /// A fixed or streaming response that would retain more is rejected instead of being kept until WINDOW_UPDATE.
  /// The connection-wide HttpServerConfig::maxOutboundBufferBytes limit still applies across all streams.
  /// Default: 4 MiB.
  uint32_t maxStreamPendingBytes{4U << 20U};

  /// Maximum number of streams that can be created over the lifetime of a connection.
  /// After this limit, the connection will be gracefully closed with GOAWAY.
  /// Must be at least 1.
  /// Default: 1,000,000 streams.
  uint32_t maxStreamsPerConnection{1000000U};

  /// Server only: number of streams of the client ended by a reset from which the connection is closed with
  /// ENHANCE_YOUR_CALM (RFC 9113 §10.5), once they are more than half of the streams it opened. A client resetting each
  /// stream as soon as it opens it ("Rapid Reset", CVE-2023-44487), or making the server reset it ("MadeYouReset",
  /// CVE-2025-8671), never has many open streams: maxConcurrentStreams does not bound it, and only
  /// maxStreamsPerConnection ends its connection.
  /// Counted: the RST_STREAM frames of the client on its open streams, and the resets of the server for an error of
  /// the client (a malformed request, a stream refused, a flow-control error...), a stream counting once. Not counted:
  /// the NO_ERROR reset ending a request answered before its end (RFC 9113 §8.1), the resets for a failure of the
  /// server (INTERNAL_ERROR) or of a CONNECT target (CONNECT_ERROR), and a reset of a stream already answered.
  /// std::numeric_limits<uint32_t>::max() means no limit: a connection has at most 2^30 streams of the client.
  /// Default: 1000.
  uint32_t maxResetStreams{1000U};

  // ============================
  // Priority (RFC 9218 / RFC 9113 §5.3)
  // ============================

  /// Maximum depth of the priority dependency tree.
  /// Limits resource usage for malicious deep dependency chains.
  /// Default: 256.
  uint16_t maxPriorityTreeDepth{256U};

  // ============================
  // HPACK
  // ============================

  /// Client only: whether the :path of the requests is added to the HPACK dynamic table. A path sent again is then
  /// encoded in about one byte instead of its literal, but each new path is inserted in the dynamic tables of both
  /// endpoints, evicting the fields that repeat in each request, and makes the next lookups of the encoder slower.
  /// Enable it for a client repeating a few paths (polling); leave it off, like nghttp2 and Chromium, for one whose
  /// paths differ (resource identifiers, queries).
  /// Default: false.
  bool indexRequestPath{false};
};

}  // namespace aeronet
