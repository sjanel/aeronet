#include "aeronet/websocket-client-handshake.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "aeronet/base64.hpp"
#include "aeronet/http-header-value-reverse-tokens-iterator.hpp"
#include "aeronet/secure-random.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/upgrade-handler.hpp"
#include "aeronet/websocket-config.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-upgrade.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/websocket-deflate.hpp"
#endif

namespace aeronet::websocket {

namespace {

// Tells whether 'protocol', the subprotocol selected by the server, is one of the comma-separated 'offeredProtocols',
// compared byte for byte: the server answers with the spelling of the client (RFC 6455 §4.1, §4.2.2).
bool IsOfferedProtocol(std::string_view protocol, std::string_view offeredProtocols) {
  for (http::HeaderValueReverseTokensIterator it(TrimOws(offeredProtocols)); it.hasNext();) {
    if (it.next() == protocol) {
      return true;
    }
  }
  return false;
}

}  // namespace

WebSocketKey GenerateWebSocketKey() {
  static constexpr std::size_t kNbKeyBytes = 16;
  static_assert(B64EncodedLen(kNbKeyBytes) == WebSocketKey{}.size());

  std::array<std::byte, kNbKeyBytes> nonce;
  FillSecureRandomBytes(std::span<std::byte>(nonce));
  WebSocketKey key;
  B64Encode(std::string_view(reinterpret_cast<const char*>(nonce.data()), nonce.size()), key.data(),
            key.data() + key.size());
  return key;
}

UpgradeResponseValidation ValidateUpgradeResponse(const UpgradeResponseHeaders& headers, std::string_view key,
                                                  std::string_view offeredProtocols,
                                                  [[maybe_unused]] const WebSocketConfig& config) {
  UpgradeResponseValidation result;
  // RFC 6455 §4.1, points 2 to 6 of the validation of the server's opening handshake.
  if (!CaseInsensitiveEqual(TrimOws(headers.upgrade), UpgradeValue)) {
    result.pErrorMessage = "Upgrade header of the response is not 'websocket'";
    return result;
  }
  if (!upgrade::ConnectionContainsUpgrade(TrimOws(headers.connection))) {
    result.pErrorMessage = "Connection header of the response does not contain 'upgrade'";
    return result;
  }
  const B64EncodedSha1 expectedAccept = ComputeWebSocketAccept(key);
  if (TrimOws(headers.accept) != std::string_view(expectedAccept.data(), expectedAccept.size())) {
    result.pErrorMessage = "Sec-WebSocket-Accept of the response does not match the key of the request";
    return result;
  }
  const std::string_view protocol = TrimOws(headers.protocol);
  if (!protocol.empty() && (protocol.contains(',') || !IsOfferedProtocol(protocol, offeredProtocols))) {
    result.pErrorMessage = "Sec-WebSocket-Protocol of the response is not one of the offered subprotocols";
    return result;
  }
  // Each extension of the response must answer an offer of the request, once (RFC 6455 §9.1).
  for (http::HeaderValueReverseTokensIterator it(TrimOws(headers.extensions)); it.hasNext();) {
    const std::string_view extension = it.next();
    if (extension.empty()) {
      continue;  // empty list element (RFC 9110 §5.6.1)
    }
#ifdef AERONET_ENABLE_ZLIB
    if (config.deflateConfig.enabled && !result.deflateParams.hasValue() && IsPermessageDeflate(extension)) {
      result.deflateParams = ParseDeflateResponse(extension, config.deflateConfig);
      if (result.deflateParams.hasValue()) {
        continue;
      }
      result.pErrorMessage = "Sec-WebSocket-Extensions of the response holds invalid permessage-deflate parameters";
      return result;
    }
#endif
    result.pErrorMessage = "Sec-WebSocket-Extensions of the response holds an extension that was not offered";
    return result;
  }
  return result;
}

}  // namespace aeronet::websocket
