#pragma once

#include <string_view>

#include "flat-hash-map.hpp"

namespace aeronet {

using HttpHeaders = flat_hash_map<std::string_view, std::string_view>;

struct HttpRequest {
  std::string_view method;
  std::string_view target;
  std::string_view version;
  HttpHeaders headers;
  std::string_view body;
  std::string_view alpnProtocol;  // Selected ALPN protocol (if any) for this connection, empty if none/unsupported
  std::string_view tlsCipher;     // Negotiated cipher suite (empty if not TLS)
  std::string_view tlsVersion;    // Negotiated TLS protocol version (e.g. TLSv1.3) empty if not TLS

  [[nodiscard]] std::string_view findHeader(std::string_view key) const;
};

}  // namespace aeronet