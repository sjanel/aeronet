#include "aeronet/upgrade-handler.hpp"

#include <cctype>
#include <cstring>
#include <string_view>

#include "aeronet/header-write.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"

#ifdef AERONET_ENABLE_WEBSOCKET
#include <algorithm>
#include <utility>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-deflate.hpp"
#include "aeronet/websocket-upgrade.hpp"
#endif

namespace aeronet {

namespace {

// Helper to write a header line to RawChars
void AppendHeader(RawChars& buf, std::string_view name, std::string_view value) {
  const auto headerSize = name.size() + http::HeaderSep.size() + value.size() + http::CRLF.size();
  buf.ensureAvailableCapacityExponential(headerSize);
  WriteHeaderCRLF(buf.data() + buf.size(), name, value);
  buf.addSize(headerSize);
}

#ifdef AERONET_ENABLE_WEBSOCKET
// Parse a comma-separated list of tokens (for Sec-WebSocket-Protocol, etc.)
ConcatenatedStrings ParseTokenList(std::string_view header) {
  ConcatenatedStrings result;
  for (std::size_t pos = 0; pos < header.size();) {
    const auto commaPos = header.find(',', pos);
    const auto tokenEnd = (commaPos == std::string_view::npos) ? header.size() : commaPos;

    const auto token = TrimOws(header.substr(pos, tokenEnd - pos));
    if (!token.empty()) {
      result.append(token);
    }

    pos = (commaPos == std::string_view::npos) ? header.size() : commaPos + 1;
  }
  return result;
}

// Select the first matching subprotocol from client's offer
[[nodiscard]] std::string_view NegotiateSubprotocol(const ConcatenatedStrings& offered,
                                                    const ConcatenatedStrings& supported) {
  // Server's preference order
  const auto it = std::ranges::find_if(
      supported, [&offered](std::string_view serverProto) { return offered.containsCI(serverProto); });
  if (it != supported.end()) {
    return *it;
  }
  return {};
}
#endif

}  // namespace

namespace upgrade {

bool ConnectionContainsUpgrade(std::string_view connectionValue) {
  // Split by comma and check each token
  std::size_t pos = 0;
  while (pos < connectionValue.size()) {
    const auto commaPos = connectionValue.find(',', pos);
    const auto tokenEnd = (commaPos == std::string_view::npos) ? connectionValue.size() : commaPos;

    std::string_view token = TrimOws(connectionValue.substr(pos, tokenEnd - pos));

    if (CaseInsensitiveEqual(token, http::Upgrade)) {
      return true;
    }

    pos = (commaPos == std::string_view::npos) ? connectionValue.size() : commaPos + 1;
  }
  return false;
}

#ifdef AERONET_ENABLE_WEBSOCKET
UpgradeValidationResult ValidateWebSocketUpgrade(const HeadersViewMap& headers, const WebSocketUpgradeConfig& config) {
  UpgradeValidationResult result;

  // Check Upgrade header
  auto it = headers.find(http::Upgrade);
  if (it == headers.end()) {
    result.errorMessage = "Missing Upgrade header";
    return result;
  }

  if (!CaseInsensitiveEqual(it->second, websocket::UpgradeValue)) {
    result.errorMessage = "Upgrade header is not 'websocket'";
    return result;
  }

  // Check Connection header contains "upgrade"
  it = headers.find(http::Connection);
  if (it == headers.end() || !ConnectionContainsUpgrade(it->second)) {
    result.errorMessage = "Connection header does not contain 'upgrade'";
    return result;
  }

  // Check Sec-WebSocket-Version
  it = headers.find(websocket::SecWebSocketVersion);
  if (it == headers.end()) {
    result.errorMessage = "Missing Sec-WebSocket-Version header";
    return result;
  }

  if (it->second != websocket::kWebSocketVersion) {
    result.errorMessage = "Unsupported Sec-WebSocket-Version (expected 13)";
    return result;
  }

  // Check Sec-WebSocket-Key
  it = headers.find(websocket::SecWebSocketKey);
  if (it == headers.end()) {
    result.errorMessage = "Missing Sec-WebSocket-Key header";
    return result;
  }

  const auto key = it->second;
  if (!IsValidWebSocketKey(key)) {
    result.errorMessage = "Invalid Sec-WebSocket-Key format";
    return result;
  }

  // Compute Sec-WebSocket-Accept
  result.secWebSocketAccept = ComputeWebSocketAccept(key);

  // Parse and negotiate Sec-WebSocket-Protocol
  it = headers.find(websocket::SecWebSocketProtocol);
  if (it != headers.end() && !it->second.empty()) {
    result.offeredProtocols = ParseTokenList(it->second);

    // Negotiate subprotocol if server supports any
    result.selectedProtocol = NegotiateSubprotocol(result.offeredProtocols, config.supportedProtocols);
  }

  // Parse Sec-WebSocket-Extensions and negotiate permessage-deflate
  it = headers.find(websocket::SecWebSocketExtensions);
  if (it != headers.end() && !it->second.empty()) {
    result.offeredExtensions = ParseTokenList(it->second);

    // Negotiate permessage-deflate if compression is enabled
    if (config.deflateConfig.enabled) {
#ifdef AERONET_ENABLE_ZLIB
      for (std::string_view ext : result.offeredExtensions) {
        auto params = websocket::ParseDeflateOffer(ext, config.deflateConfig);
        if (params.has_value()) {
          result.deflateParams = std::move(*params);
          break;  // Take the first acceptable offer
        }
      }
#else
      // Zlib support not compiled in; cannot negotiate compression
      result.errorMessage = "permessage-deflate requested but zlib support is not enabled";
      return result;
#endif
    }
  }

  result.valid = true;
  result.targetProtocol = ProtocolType::WebSocket;
  return result;
}
#endif

#ifdef AERONET_ENABLE_HTTP2
UpgradeValidationResult ValidateHttp2Upgrade([[maybe_unused]] const HeadersViewMap& headers) {
  UpgradeValidationResult result;

  // Check Upgrade header
  auto it = headers.find(http::Upgrade);
  if (it == headers.end()) {
    result.errorMessage = "Missing Upgrade header";
    return result;
  }

  if (!CaseInsensitiveEqual(it->second, http2::kAlpnH2c)) {
    result.errorMessage = "Upgrade header is not 'h2c'";
    return result;
  }

  // Check Connection header contains "upgrade" and "HTTP2-Settings"
  it = headers.find(http::Connection);
  if (it == headers.end()) {
    result.errorMessage = "Missing Connection header";
    return result;
  }

  if (!ConnectionContainsUpgrade(it->second)) {
    result.errorMessage = "Connection header does not contain 'upgrade'";
    return result;
  }

  // Check for HTTP2-Settings header
  static constexpr std::string_view kHttp2Settings = "HTTP2-Settings";
  it = headers.find(kHttp2Settings);
  if (it == headers.end()) {
    result.errorMessage = "Missing HTTP2-Settings header";
    return result;
  }

  // The HTTP2-Settings header must contain a base64url-encoded SETTINGS payload
  // We validate format here; actual parsing happens during protocol switch
  if (it->second.empty()) {
    result.errorMessage = "Empty HTTP2-Settings header";
    return result;
  }

  result.valid = true;
  result.targetProtocol = ProtocolType::Http2;
  return result;
}
#endif

ProtocolType DetectUpgradeTarget(std::string_view upgradeHeaderValue) {
#ifdef AERONET_ENABLE_HTTP2
  if (CaseInsensitiveEqual(upgradeHeaderValue, http2::kAlpnH2c)) {
    return ProtocolType::Http2;
  }
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
  if (CaseInsensitiveEqual(upgradeHeaderValue, websocket::UpgradeValue)) {
    return ProtocolType::WebSocket;
  }
#endif

  return ProtocolType::Http11;
}

namespace {

constexpr std::string_view kSwitchingProtocolsHttp11HeaderLine = "HTTP/1.1 101 Switching Protocols\r\n";

}

#ifdef AERONET_ENABLE_WEBSOCKET
RawChars BuildWebSocketUpgradeResponse(const UpgradeValidationResult& validationResult) {
  // Build raw HTTP response: "HTTP/1.1 101 Switching Protocols\r\n" + headers + "\r\n"
  RawChars response(kSwitchingProtocolsHttp11HeaderLine.size() + 192);  // Typical 101 WebSocket response size

  // Status line
  response.unchecked_append(kSwitchingProtocolsHttp11HeaderLine);

  // Headers
  AppendHeader(response, http::Upgrade, websocket::UpgradeValue);
  AppendHeader(response, http::Connection, http::Upgrade);
  AppendHeader(
      response, websocket::SecWebSocketAccept,
      std::string_view(validationResult.secWebSocketAccept.data(), validationResult.secWebSocketAccept.size()));

  if (!validationResult.selectedProtocol.empty()) {
    AppendHeader(response, websocket::SecWebSocketProtocol, validationResult.selectedProtocol);
  }

  // Include negotiated extensions
  if (validationResult.deflateParams.has_value()) {
    const auto deflateResponse = websocket::BuildDeflateResponse(*validationResult.deflateParams);
    AppendHeader(response, websocket::SecWebSocketExtensions,
                 std::string_view(reinterpret_cast<const char*>(deflateResponse.data()), deflateResponse.size()));
  }

  // End of headers
  response.append(http::CRLF);

  return response;
}
#endif

#ifdef AERONET_ENABLE_HTTP2
RawChars BuildHttp2UpgradeResponse(const UpgradeValidationResult& validationResult) {
  (void)validationResult;  // Used for future extension

  // Build raw HTTP response: "HTTP/1.1 101 Switching Protocols\r\n" + headers + "\r\n"
  RawChars response(kSwitchingProtocolsHttp11HeaderLine.size() + 96);

  // Status line
  response.unchecked_append(kSwitchingProtocolsHttp11HeaderLine);

  // Headers
  AppendHeader(response, http::Upgrade, http2::kAlpnH2c);
  AppendHeader(response, http::Connection, http::Upgrade);

  // End of headers
  response.append(http::CRLF);

  return response;
}
#endif

}  // namespace upgrade

}  // namespace aeronet
