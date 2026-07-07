#pragma once

#include <cstdint>
#include <string_view>

namespace aeronet {

// Error category returned by HttpClient for a request that could not be completed. This is the value-based
// counterpart to the (setup-only) HttpClientException: every per-request runtime failure -- DNS/connect
// failure, timeout, TLS error, malformed/oversized response, ... -- is reported as one of these codes
// inside an HttpClientResult, never thrown. A non-2xx HTTP status is NOT an error: it is a normal
// HttpResponse held in the success state.
//
// The code is intentionally a single byte (no allocation, no message string): the human-readable
// description comes from ErrcToStr(), and request-specific context (which origin) is reconstructible by
// the caller, which still holds the URL it asked for.
enum class HttpClientErrc : uint8_t {
  invalidUrl,           // malformed / unsupported request URL or redirect Location
  connectFailed,        // DNS resolution or TCP connect failure (includes the connect deadline)
  tlsError,             // TLS handshake failed
  timeout,              // I/O deadline exceeded (TLS handshake, request write or response read)
  writeError,           // transport write failure
  connectionClosed,     // peer closed the connection before a complete response was received
  malformedResponse,    // response could not be parsed, or exceeded maxResponseBytes
  protocolUnsupported,  // peer negotiated an application protocol we have no engine for (e.g. ALPN h2)
  proxyError,           // the forward proxy refused or failed to establish the CONNECT tunnel
  ioError,              // event-loop registration or other internal I/O failure
};

// Stable, human-readable description of an error code (static storage, no allocation). Intended for logs
// and diagnostics; callers branch on the enum value, not on this text.
[[nodiscard]] constexpr std::string_view ErrcToStr(HttpClientErrc errc) noexcept {
  switch (errc) {
    case HttpClientErrc::invalidUrl:
      return "malformed or unsupported URL";
    case HttpClientErrc::connectFailed:
      return "connection failed";
    case HttpClientErrc::tlsError:
      return "TLS handshake failed";
    case HttpClientErrc::timeout:
      return "operation timed out";
    case HttpClientErrc::writeError:
      return "transport write failed";
    case HttpClientErrc::connectionClosed:
      return "connection closed before a complete response was received";
    case HttpClientErrc::malformedResponse:
      return "malformed or oversized response";
    case HttpClientErrc::protocolUnsupported:
      return "negotiated application protocol is not supported";
    case HttpClientErrc::proxyError:
      return "forward proxy failed to establish the CONNECT tunnel";
    case HttpClientErrc::ioError:
      return "internal I/O error";
  }
  return "unknown error";
}

}  // namespace aeronet
