#pragma once

#include <cstdint>
#include <string_view>

namespace aeronet::http2 {

// HTTP/2 Protocol Constants (RFC 9113, formerly RFC 7540)
// ========================================================

// Connection preface: client must send this magic string first (RFC 9113 §3.4)
// "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
inline constexpr std::string_view kConnectionPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
inline constexpr std::size_t kConnectionPrefaceSize = 24;

// ALPN protocol identifier for HTTP/2 over TLS
inline constexpr std::string_view kAlpnH2 = "h2";

// ALPN protocol identifier for HTTP/2 cleartext (upgrade from HTTP/1.1)
inline constexpr std::string_view kAlpnH2c = "h2c";

// HTTP/2 Frame Types (RFC 9113 §6)
// ================================
enum class FrameType : uint8_t {
  Data = 0x00,          // DATA frame - carries request/response body
  Headers = 0x01,       // HEADERS frame - carries header fields
  Priority = 0x02,      // PRIORITY frame - specifies stream priority (deprecated in RFC 9113)
  RstStream = 0x03,     // RST_STREAM frame - terminates a stream
  Settings = 0x04,      // SETTINGS frame - configuration parameters
  PushPromise = 0x05,   // PUSH_PROMISE frame - server push (rarely used, deprecated in some contexts)
  Ping = 0x06,          // PING frame - connection liveness/RTT measurement
  GoAway = 0x07,        // GOAWAY frame - graceful connection shutdown
  WindowUpdate = 0x08,  // WINDOW_UPDATE frame - flow control
  Continuation = 0x09,  // CONTINUATION frame - continuation of header block
  // 0x0A-0xFF reserved for extensions
};

// HTTP/2 Error Codes (RFC 9113 §7)
// ================================
// Note: these codes are 32-bit values on the wire (RFC 9113). Keep the
// underlying type as `uint32_t` intentionally to match the protocol's
// on-the-wire representation and to avoid accidental truncation when
// serializing/deserializing frames.
enum class ErrorCode : uint32_t {  // NOLINT(performance-enum-size)
  NoError = 0x00,                  // Graceful shutdown
  ProtocolError = 0x01,            // Protocol error detected
  InternalError = 0x02,            // Implementation fault
  FlowControlError = 0x03,         // Flow control limits exceeded
  SettingsTimeout = 0x04,          // Settings not acknowledged in time
  StreamClosed = 0x05,             // Frame received for closed stream
  FrameSizeError = 0x06,           // Frame size incorrect
  RefusedStream = 0x07,            // Stream not processed
  Cancel = 0x08,                   // Stream cancelled
  CompressionError = 0x09,         // HPACK decompression failed
  ConnectError = 0x0A,             // TCP connection error for CONNECT
  EnhanceYourCalm = 0x0B,          // Excessive load
  InadequateSecurity = 0x0C,       // Negotiated TLS parameters inadequate
  Http11Required = 0x0D,           // HTTP/1.1 required for this request
};

// HTTP/2 Settings Parameters (RFC 9113 §6.5.2)
// =============================================
// Note: SETTINGS parameters are 16-bit identifiers on the wire (RFC 9113 §6.5.2).
// Keep the underlying type as `uint16_t` intentionally to match the protocol's
// on-the-wire representation and to avoid accidental truncation when
// serializing/deserializing frames.
enum class SettingsParameter : uint16_t {  // NOLINT(performance-enum-size)
  HeaderTableSize = 0x01,                  // HPACK dynamic table size (default: 4096)
  EnablePush = 0x02,                       // Whether server push is permitted (default: 1)
  MaxConcurrentStreams = 0x03,             // Maximum concurrent streams (default: unlimited)
  InitialWindowSize = 0x04,                // Initial flow control window size (default: 65535)
  MaxFrameSize = 0x05,                     // Maximum frame payload size (default: 16384)
  MaxHeaderListSize = 0x06,                // Maximum size of header list (default: unlimited)
};

// HTTP/2 Frame Flags (RFC 9113 §6)
// ================================
namespace flags {

// DATA frame flags (§6.1)
inline constexpr uint8_t DataEndStream = 0x01;  // END_STREAM: last frame for this stream
inline constexpr uint8_t DataPadded = 0x08;     // PADDED: frame is padded

// HEADERS frame flags (§6.2)
inline constexpr uint8_t HeadersEndStream = 0x01;   // END_STREAM: last frame for this stream
inline constexpr uint8_t HeadersEndHeaders = 0x04;  // END_HEADERS: no CONTINUATION follows
inline constexpr uint8_t HeadersPadded = 0x08;      // PADDED: frame is padded
inline constexpr uint8_t HeadersPriority = 0x20;    // PRIORITY: priority fields present

// SETTINGS frame flags (§6.5)
inline constexpr uint8_t SettingsAck = 0x01;  // ACK: acknowledging peer's SETTINGS

// PING frame flags (§6.7)
inline constexpr uint8_t PingAck = 0x01;  // ACK: response to PING

// CONTINUATION frame flags (§6.10)
inline constexpr uint8_t ContinuationEndHeaders = 0x04;  // END_HEADERS: end of header block

}  // namespace flags

// HTTP/2 Default Values (RFC 9113 §6.5.2)
// =======================================
inline constexpr uint32_t kDefaultHeaderTableSize = 4096;
inline constexpr uint32_t kDefaultEnablePush = 1;
inline constexpr uint32_t kDefaultMaxConcurrentStreams = 100;  // RFC says unlimited, but we set a reasonable default
inline constexpr uint32_t kDefaultInitialWindowSize = 65535;
inline constexpr uint32_t kDefaultMaxFrameSize = 16384;
inline constexpr uint32_t kDefaultMaxHeaderListSize = 8192;  // RFC says unlimited, but we set a reasonable default

// HTTP/2 Limits (RFC 9113)
// ========================
inline constexpr uint32_t kMinMaxFrameSize = 16384;     // Minimum allowed SETTINGS_MAX_FRAME_SIZE
inline constexpr uint32_t kMaxMaxFrameSize = 16777215;  // Maximum allowed SETTINGS_MAX_FRAME_SIZE (2^24 - 1)
inline constexpr uint32_t kMaxWindowSize = 2147483647;  // Maximum flow control window size (2^31 - 1)
inline constexpr uint32_t kMaxStreamId = 2147483647;    // Maximum stream identifier (2^31 - 1)

// Frame header size is always 9 bytes
inline constexpr std::size_t kFrameHeaderSize = 9;

// Stream identifier constants
inline constexpr uint32_t kConnectionStreamId = 0;  // Stream 0 is the connection control stream

// Check if a stream ID is valid for client-initiated streams (odd numbers)
[[nodiscard]] constexpr bool IsClientStream(uint32_t streamId) noexcept { return (streamId & 1) != 0; }

// Check if a stream ID is valid for server-initiated streams (even numbers, non-zero)
[[nodiscard]] constexpr bool IsServerStream(uint32_t streamId) noexcept { return streamId != 0 && (streamId & 1) == 0; }

// HTTP/2 Stream States (RFC 9113 §5.1)
// ====================================
enum class StreamState : uint8_t {
  Idle,              // Stream not yet opened
  ReservedLocal,     // Reserved (local) for server push
  ReservedRemote,    // Reserved (remote) for server push
  Open,              // Stream is active
  HalfClosedLocal,   // Local side closed (sent END_STREAM)
  HalfClosedRemote,  // Remote side closed (received END_STREAM)
  Closed,            // Stream is closed
};

}  // namespace aeronet::http2
