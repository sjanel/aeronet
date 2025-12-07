#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/static-concatenated-strings.hpp"

namespace aeronet {

// Optimized pack of TLS str information in one allocated storage containing the following information:
// negotiated ALPN protocol (if any)
// negotiated TLS cipher suite (if TLS)
// negotiated TLS protocol version string
// RFC2253 formatted subject if client cert present
class TLSInfo {
 public:
  using Parts = StaticConcatenatedStrings<4, uint32_t>;

  TLSInfo() noexcept = default;

  explicit TLSInfo(Parts parts) noexcept : _parts(std::move(parts)) {}

  [[nodiscard]] std::string_view selectedAlpn() const noexcept { return _parts[0]; }

  [[nodiscard]] std::string_view negotiatedCipher() const noexcept { return _parts[1]; }

  [[nodiscard]] std::string_view negotiatedVersion() const noexcept { return _parts[2]; }

  [[nodiscard]] std::string_view peerSubject() const noexcept { return _parts[3]; }

 private:
  Parts _parts;
};

}  // namespace aeronet