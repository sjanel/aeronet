#include "aeronet/upgrade-handler.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/sv-to-sv-map.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-deflate.hpp"
#include "aeronet/websocket-endpoint.hpp"
#include "aeronet/websocket-handler.hpp"
#include "aeronet/websocket-upgrade.hpp"

namespace aeronet {

namespace {

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

UpgradeValidationResult ValidateWebSocketUpgrade(const SvToSvMap& headers, const WebSocketUpgradeConfig& config) {
  UpgradeValidationResult result;

  // Check Upgrade header
  auto it = headers.find(http::Upgrade);
  if (it == headers.end()) {
    result.pErrorMessage = "Missing Upgrade header";
    return result;
  }

  if (!CaseInsensitiveEqual(it->second, websocket::UpgradeValue)) {
    result.pErrorMessage = "Upgrade header is not 'websocket'";
    return result;
  }

  // Check Connection header contains "upgrade"
  it = headers.find(http::Connection);
  if (it == headers.end() || !ConnectionContainsUpgrade(it->second)) {
    result.pErrorMessage = "Connection header does not contain 'upgrade'";
    return result;
  }

  // Check Sec-WebSocket-Version
  it = headers.find(websocket::SecWebSocketVersion);
  if (it == headers.end()) {
    result.pErrorMessage = "Missing Sec-WebSocket-Version header";
    return result;
  }

  if (it->second != websocket::kWebSocketVersion) {
    result.pErrorMessage = "Unsupported Sec-WebSocket-Version (expected 13)";
    return result;
  }

  // Check Sec-WebSocket-Key
  it = headers.find(websocket::SecWebSocketKey);
  if (it == headers.end()) {
    result.pErrorMessage = "Missing Sec-WebSocket-Key header";
    return result;
  }

  const auto key = it->second;
  if (!IsValidWebSocketKey(key)) {
    result.pErrorMessage = "Invalid Sec-WebSocket-Key format";
    return result;
  }

  // Compute Sec-WebSocket-Accept
  result.secWebSocketAccept = ComputeWebSocketAccept(key);

  // Parse and negotiate Sec-WebSocket-Protocol
  it = headers.find(websocket::SecWebSocketProtocol);
  if (it != headers.end() && !it->second.empty()) {
    // Negotiate subprotocol if server supports any
    result.selectedProtocol = NegotiateSubprotocol(ParseTokenList(it->second), config.supportedProtocols);
  }

  // Parse Sec-WebSocket-Extensions and negotiate permessage-deflate
  it = headers.find(websocket::SecWebSocketExtensions);
  if (it != headers.end() && !it->second.empty()) {
    // Negotiate permessage-deflate if compression is enabled
    if (config.deflateConfig.enabled) {
#ifdef AERONET_ENABLE_ZLIB
      for (std::string_view ext : ParseTokenList(it->second)) {
        result.deflateParams = websocket::ParseDeflateOffer(ext, config.deflateConfig);
        if (result.deflateParams.hasValue()) {
          break;  // Take the first acceptable offer
        }
      }
#else
      // Zlib support not compiled in; cannot negotiate compression
      result.pErrorMessage = "permessage-deflate requested but zlib support is not enabled";
      return result;
#endif
    }
  }

  result.targetProtocol = ProtocolType::WebSocket;
  return result;
}

ProtocolType DetectUpgradeTarget([[maybe_unused]] std::string_view upgradeHeaderValue) {
  // "h2c" is intentionally not an upgrade target (RFC 9113 §3.1 deprecated it): such requests stay on HTTP/1.1.
  if (CaseInsensitiveEqual(upgradeHeaderValue, websocket::UpgradeValue)) {
    return ProtocolType::WebSocket;
  }

  return ProtocolType::Http11;
}

namespace {

constexpr std::string_view kSwitchingProtocolsHttp11HeaderLine = "HTTP/1.1 101 Switching Protocols\r\n";

}  // namespace

bool IsSameOriginRequest(std::string_view origin, std::string_view host) {
  if (origin.empty()) {
    return true;
  }
  static constexpr std::string_view kSchemeSep = "://";
  const auto schemeEnd = origin.find(kSchemeSep);
  if (schemeEnd == std::string_view::npos) {
    return false;  // "null" (opaque origin) or malformed
  }
  const std::string_view scheme = origin.substr(0, schemeEnd);
  std::string_view defaultPort;
  if (CaseInsensitiveEqual(scheme, "https")) {
    defaultPort = ":443";
  } else if (CaseInsensitiveEqual(scheme, "http")) {
    defaultPort = ":80";
  }
  const auto withoutDefaultPort = [defaultPort](std::string_view authority) {
    if (!defaultPort.empty() && authority.ends_with(defaultPort)) {
      authority.remove_suffix(defaultPort.size());
    }
    return authority;
  };
  return CaseInsensitiveEqual(withoutDefaultPort(origin.substr(schemeEnd + kSchemeSep.size())),
                              withoutDefaultPort(host));
}

std::size_t ComputeWebSocketUpgradeResponseSize(const UpgradeValidationResult& validationResult) {
  std::size_t responseSz =
      kSwitchingProtocolsHttp11HeaderLine.size() +
      http::HeaderSize(http::Upgrade.size(), websocket::UpgradeValue.size()) +
      http::HeaderSize(http::Connection.size(), http::Upgrade.size()) +
      http::HeaderSize(websocket::SecWebSocketAccept.size(), validationResult.secWebSocketAccept.size());

  if (!validationResult.selectedProtocol.empty()) {
    responseSz += http::HeaderSize(websocket::SecWebSocketProtocol.size(), validationResult.selectedProtocol.size());
  }
  if (validationResult.deflateParams.hasValue()) {
    const auto& params = validationResult.deflateParams;
    const auto valueSize =
        ComputeDeflateResponseSize(params, ndigits(params.serverMaxWindowBits), ndigits(params.clientMaxWindowBits));

    responseSz +=
        websocket::SecWebSocketExtensions.size() + http::HeaderSep.size() + valueSize + http::DoubleCRLF.size();
  } else {
    responseSz += http::CRLF.size();
  }
  return responseSz;
}

// Build raw HTTP response: "HTTP/1.1 101 Switching Protocols\r\n" + headers + "\r\n"
void BuildWebSocketUpgradeResponse(const UpgradeValidationResult& validationResult, char* pData) {
  // Status line
  pData = AppendFixed<kSwitchingProtocolsHttp11HeaderLine>(pData);

  // Headers
  pData = WriteHeaderCRLF(http::Upgrade, websocket::UpgradeValue, pData);
  pData = WriteHeaderCRLF(http::Connection, http::Upgrade, pData);
  pData = WriteHeaderCRLF(
      websocket::SecWebSocketAccept,
      std::string_view(validationResult.secWebSocketAccept.data(), validationResult.secWebSocketAccept.size()), pData);

  if (!validationResult.selectedProtocol.empty()) {
    pData = WriteHeaderCRLF(websocket::SecWebSocketProtocol, validationResult.selectedProtocol, pData);
  }

  // Include negotiated extensions
  if (validationResult.deflateParams.hasValue()) {
    const auto& params = validationResult.deflateParams;

    pData = AppendFixed<websocket::SecWebSocketExtensions>(pData);
    pData = AppendFixed<http::HeaderSep>(pData);

    pData = websocket::BuildDeflateResponse(params, pData);

    pData = AppendFixed<http::DoubleCRLF>(pData);
  } else {
    pData = AppendFixed<http::CRLF>(pData);
  }
}

WebSocketAcceptResult AcceptWebSocketUpgrade(const HttpRequestView& request, const SvToSvMap& requestHeaders,
                                             const WebSocketEndpoint& endpoint,
                                             websocket::SharedDeflateStreams& sharedStreams) {
  WebSocketAcceptResult result;
  const WebSocketUpgradeConfig upgradeConfig{endpoint.supportedProtocols, endpoint.config.deflateConfig};
  const UpgradeValidationResult validation = ValidateWebSocketUpgrade(requestHeaders, upgradeConfig);
  if (!validation.valid()) {
    result.pErrorMessage = validation.pErrorMessage;
    result.status = WebSocketAcceptResult::Status::Invalid;
    return result;
  }

  if (endpoint.factory) {
    try {
      result.handler = endpoint.factory(request);
    } catch (const std::exception& ex) {
      log::error("Exception in WebSocket handler factory: {}", ex.what());
      result.status = WebSocketAcceptResult::Status::Failed;
      return result;
    } catch (...) {
      log::error("Unknown exception in WebSocket handler factory");
      result.status = WebSocketAcceptResult::Status::Failed;
      return result;
    }
    if (!result.handler) {
      result.status = WebSocketAcceptResult::Status::Refused;
      return result;
    }
  } else {
    auto config = endpoint.config;
    config.isServerSide = true;
    result.handler = std::make_unique<websocket::WebSocketHandler>(std::move(config));
  }
  if (!result.handler->hasCompression() && validation.deflateParams.hasValue()) {
    // Compression was negotiated but the handler (from the factory, which keeps its callbacks) was created without it.
    result.handler->enableCompression(validation.deflateParams, &sharedStreams);
  }

  const std::size_t responseSize = ComputeWebSocketUpgradeResponseSize(validation);
  result.response.ensureAvailableCapacity(responseSize);
  BuildWebSocketUpgradeResponse(validation, result.response.data());
  result.response.setSize(responseSize);
  return result;
}

}  // namespace upgrade

}  // namespace aeronet
