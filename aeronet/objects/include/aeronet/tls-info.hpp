#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "aeronet/static-concatenated-strings.hpp"

namespace aeronet {

struct TLSInfo {
  // Optimized pack of TLS str information in one allocated storage containing the following information:
  // negotiated ALPN protocol (if any)
  // negotiated TLS cipher suite (if TLS)
  // negotiated TLS protocol version string
  // RFC2253 formatted subject if client cert present
  using Parts = StaticConcatenatedStrings<4, uint32_t>;

  [[nodiscard]] std::string_view selectedAlpn() const noexcept { return parts[0]; }

  [[nodiscard]] std::string_view negotiatedCipher() const noexcept { return parts[1]; }

  [[nodiscard]] std::string_view negotiatedVersion() const noexcept { return parts[2]; }

  [[nodiscard]] std::string_view peerSubject() const noexcept { return parts[3]; }

  std::chrono::steady_clock::time_point handshakeStart;  // TLS handshake start time (steady clock)

  Parts parts;
};

}  // namespace aeronet