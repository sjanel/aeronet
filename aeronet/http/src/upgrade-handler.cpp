#include "aeronet/upgrade-handler.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/base64-encode.hpp"
#include "aeronet/concatenated-strings.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sha1.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-deflate.hpp"

namespace aeronet {

namespace {

// Check if a character is valid base64
[[nodiscard]] bool IsBase64Char(char ch) noexcept {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' ||
         ch == '=';
}

// Helper to write a header line to RawChars
void AppendHeader(RawChars& buf, std::string_view name, std::string_view value) {
  buf.ensureAvailableCapacityExponential(name.size() + http::HeaderSep.size() + value.size() + http::CRLF.size());

  buf.unchecked_append(name);
  buf.unchecked_append(http::HeaderSep);
  buf.unchecked_append(value);
  buf.unchecked_append(http::CRLF);
}

// Parse a comma-separated list of tokens (for Sec-WebSocket-Protocol, etc.)
ConcatenatedStrings ParseTokenList(std::string_view header) {
  ConcatenatedStrings result;
  std::size_t pos = 0;
  while (pos < header.size()) {
    const auto commaPos = header.find(',', pos);
    const auto tokenEnd = (commaPos == std::string_view::npos) ? header.size() : commaPos;

    auto token = TrimOws(header.substr(pos, tokenEnd - pos));
    if (!token.empty()) {
      result.append(token);
    }

    pos = (commaPos == std::string_view::npos) ? header.size() : commaPos + 1;
  }
  return result;
}

// Select the first matching subprotocol from client's offer
[[nodiscard]] std::string_view NegotiateSubprotocol(const ConcatenatedStrings& offered,
                                                    std::span<const std::string_view> supported) {
  // Server's preference order
  auto it = std::ranges::find_if(supported,
                                 [&offered](std::string_view serverProto) { return offered.containsCI(serverProto); });
  if (it != supported.end()) {
    return *it;
  }
  return {};
}

}  // namespace

namespace upgrade {

UpgradeValidationResult::B64EncodedSha1 ComputeWebSocketAccept(std::string_view key) {
  // Concatenate key with WebSocket GUID
  RawChars concat(key.size() + websocket::kWebSocketGUID.size());
  concat.unchecked_append(key);
  concat.unchecked_append(websocket::kWebSocketGUID);

  // Compute SHA-1 hash
  SHA1 sha1Ctx;
  sha1Ctx.update(concat.data(), concat.size());
  const Sha1Digest hash = sha1Ctx.final();

  static constexpr auto b64EncodedSz = B64EncodedLen(sizeof(hash));

  static_assert(b64EncodedSz == UpgradeValidationResult::B64EncodedSha1{}.size(), "Unexpected B64EncodedSha1 size");

  UpgradeValidationResult::B64EncodedSha1 ret;

  B64Encode(hash, ret.data(), ret.data() + ret.size());

  return ret;
}

bool ConnectionContainsUpgrade(std::string_view connectionValue) {
  // Split by comma and check each token
  std::size_t pos = 0;
  while (pos < connectionValue.size()) {
    const auto commaPos = connectionValue.find(',', pos);
    const auto tokenEnd = (commaPos == std::string_view::npos) ? connectionValue.size() : commaPos;

    auto token = TrimOws(connectionValue.substr(pos, tokenEnd - pos));

    if (CaseInsensitiveEqual(token, http::Upgrade)) {
      return true;
    }

    pos = (commaPos == std::string_view::npos) ? connectionValue.size() : commaPos + 1;
  }
  return false;
}

bool IsValidWebSocketKey(std::string_view key) {
  // Must be exactly 24 base64 characters (16 bytes -> 24 chars with padding)
  if (key.size() != 24) {
    return false;
  }

  // Verify all characters are valid base64
  if (!std::ranges::all_of(key, [](char ch) { return IsBase64Char(ch); })) {
    return false;
  }

  // The key should end with "==" as 16 bytes encodes to 22 chars + 2 padding
  return key[22] == '=' && key[23] == '=';
}

UpgradeValidationResult ValidateWebSocketUpgrade(const HttpRequest& request, const WebSocketUpgradeConfig& config) {
  UpgradeValidationResult result;

  // Check Upgrade header
  const auto upgradeHeader = request.headerValue(http::Upgrade);
  if (!upgradeHeader.has_value()) {
    result.errorMessage = "Missing Upgrade header";
    return result;
  }

  if (!CaseInsensitiveEqual(TrimOws(*upgradeHeader), websocket::UpgradeValue)) {
    result.errorMessage = "Upgrade header is not 'websocket'";
    return result;
  }

  // Check Connection header contains "upgrade"
  const auto connectionHeader = request.headerValue(http::Connection);
  if (!connectionHeader.has_value() || !ConnectionContainsUpgrade(*connectionHeader)) {
    result.errorMessage = "Connection header does not contain 'upgrade'";
    return result;
  }

  // Check Sec-WebSocket-Version
  const auto versionHeader = request.headerValue(websocket::SecWebSocketVersion);
  if (!versionHeader.has_value()) {
    result.errorMessage = "Missing Sec-WebSocket-Version header";
    return result;
  }

  if (TrimOws(*versionHeader) != websocket::kWebSocketVersion) {
    result.errorMessage = "Unsupported Sec-WebSocket-Version (expected 13)";
    return result;
  }

  // Check Sec-WebSocket-Key
  const auto keyHeader = request.headerValue(websocket::SecWebSocketKey);
  if (!keyHeader.has_value()) {
    result.errorMessage = "Missing Sec-WebSocket-Key header";
    return result;
  }

  const auto key = TrimOws(*keyHeader);
  if (!IsValidWebSocketKey(key)) {
    result.errorMessage = "Invalid Sec-WebSocket-Key format";
    return result;
  }

  // Compute Sec-WebSocket-Accept
  result.secWebSocketAccept = ComputeWebSocketAccept(key);

  // Parse and negotiate Sec-WebSocket-Protocol
  const auto protocolHeader = request.headerValue(websocket::SecWebSocketProtocol);
  if (protocolHeader.has_value() && !protocolHeader->empty()) {
    result.offeredProtocols = ParseTokenList(*protocolHeader);

    // Negotiate subprotocol if server supports any
    if (!config.supportedProtocols.empty()) {
      result.selectedProtocol = NegotiateSubprotocol(result.offeredProtocols, config.supportedProtocols);
    }
  }

  // Parse Sec-WebSocket-Extensions and negotiate permessage-deflate
  const auto extensionsHeader = request.headerValue(websocket::SecWebSocketExtensions);
  if (extensionsHeader.has_value() && !extensionsHeader->empty()) {
    result.offeredExtensions = ParseTokenList(*extensionsHeader);

    // Negotiate permessage-deflate if compression is enabled
    if (config.enableCompression) {
#ifdef AERONET_ENABLE_ZLIB
      for (const auto& ext : result.offeredExtensions) {
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

UpgradeValidationResult ValidateHttp2Upgrade(const HttpRequest& request) {
  UpgradeValidationResult result;

  // Check Upgrade header
  const auto upgradeHeader = request.headerValue(http::Upgrade);
  if (!upgradeHeader.has_value()) {
    result.errorMessage = "Missing Upgrade header";
    return result;
  }

  if (!CaseInsensitiveEqual(TrimOws(*upgradeHeader), http2::kAlpnH2c)) {
    result.errorMessage = "Upgrade header is not 'h2c'";
    return result;
  }

  // Check Connection header contains "upgrade" and "HTTP2-Settings"
  const auto connectionHeader = request.headerValue(http::Connection);
  if (!connectionHeader.has_value()) {
    result.errorMessage = "Missing Connection header";
    return result;
  }

  if (!ConnectionContainsUpgrade(*connectionHeader)) {
    result.errorMessage = "Connection header does not contain 'upgrade'";
    return result;
  }

  // Check for HTTP2-Settings header
  static constexpr std::string_view kHttp2Settings = "HTTP2-Settings";
  const auto settingsHeader = request.headerValue(kHttp2Settings);
  if (!settingsHeader.has_value()) {
    result.errorMessage = "Missing HTTP2-Settings header";
    return result;
  }

  // The HTTP2-Settings header must contain a base64url-encoded SETTINGS payload
  // We validate format here; actual parsing happens during protocol switch
  if (settingsHeader->empty()) {
    result.errorMessage = "Empty HTTP2-Settings header";
    return result;
  }

  result.valid = true;
  result.targetProtocol = ProtocolType::Http2;
  return result;
}

ProtocolType DetectUpgradeTarget(const HttpRequest& request) {
  const auto upgradeHeader = request.headerValue(http::Upgrade);
  if (!upgradeHeader.has_value()) {
    return ProtocolType::Http11;
  }

  const auto value = TrimOws(*upgradeHeader);

  if (CaseInsensitiveEqual(value, websocket::UpgradeValue)) {
    return ProtocolType::WebSocket;
  }

  if (CaseInsensitiveEqual(value, http2::kAlpnH2c)) {
    return ProtocolType::Http2;
  }

  return ProtocolType::Http11;
}

namespace {

constexpr std::string_view kSwitchingProtocolsHttp11HeaderLine = "HTTP/1.1 101 Switching Protocols\r\n";

}

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

}  // namespace upgrade

}  // namespace aeronet
