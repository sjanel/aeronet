#include "proxy-tunnel.hpp"

#include <cstddef>
#include <string_view>

#include "aeronet/decimal-writer.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "status-line.hpp"

namespace aeronet::internal {

void BuildProxyConnectRequest(const HttpRequest& req, RawChars& out) {
  static constexpr std::string_view kConnect = "CONNECT ";
  static constexpr std::string_view kConnectMid = " HTTP/1.1\r\nHost: ";  // between request-target and Host value

  const std::string_view host = req.host();
  const bool ipv6 = host.contains(':');
  const auto port = req.port();
  const auto portNbDigits = ndigits(port);
  const std::size_t authorityLen = host.size() + (ipv6 ? 2U : 0U) + 1U + portNbDigits;

  const auto appendAuthority = [ipv6, host, port, portNbDigits](char* pOut) {
    if (ipv6) {
      *pOut++ = '[';
    }
    pOut = Append(host, pOut);
    if (ipv6) {
      *pOut++ = ']';
    }
    *pOut++ = ':';
    return WriteUInt(pOut, port, portNbDigits);
  };

  out.clear();
  out.reserve(kConnect.size() + authorityLen + kConnectMid.size() + authorityLen + http::DoubleCRLF.size());
  char* pData = AppendFixed<kConnect>(out.data());
  pData = appendAuthority(pData);
  pData = AppendFixed<kConnectMid>(pData);
  pData = appendAuthority(pData);
  out.setEnd(AppendFixed<http::DoubleCRLF>(pData));
}

ProxyTunnelStatus CheckProxyTunnelResponse(std::string_view received) noexcept {
  const std::size_t endPos = received.find(http::DoubleCRLF);
  if (endPos == std::string_view::npos) {
    return received.size() < kMaxProxyTunnelResponse ? ProxyTunnelStatus::Incomplete : ProxyTunnelStatus::Refused;
  }
  // The status line is everything up to the first CRLF (present within [0, endPos]). Any 2xx response opens the tunnel
  // (RFC 9110 §9.3.6); anything else, or a line that is not a status line, is a proxy failure.
  const http::StatusCode code = http::ParseStatusLine(received.substr(0, received.find(http::CRLF))).code;
  return code >= http::StatusCodeOK && code < http::StatusCodeMultipleChoices ? ProxyTunnelStatus::Open
                                                                              : ProxyTunnelStatus::Refused;
}

}  // namespace aeronet::internal
