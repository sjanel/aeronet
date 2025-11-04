#pragma once

#include <cstdint>
#include <string_view>

#include "static-concatenated-strings.hpp"

namespace aeronet {

// Optimized pack of TLS str information in one allocated storage containing the following information:
// negotiated ALPN protocol (if any)
// negotiated TLS cipher suite (if TLS)
// negotiated TLS protocol version string
class TLSInfo {
 public:
  TLSInfo() noexcept = default;

  TLSInfo(std::string_view selectedAlpn, std::string_view negotiatedCipher, std::string_view negotiatedVersion)
      : _parts({selectedAlpn, negotiatedCipher, negotiatedVersion}) {}

  [[nodiscard]] std::string_view selectedAlpn() const noexcept { return _parts[0]; }

  [[nodiscard]] std::string_view negotiatedCipher() const noexcept { return _parts[1]; }

  [[nodiscard]] std::string_view negotiatedVersion() const noexcept { return _parts[2]; }

 private:
  aeronet::StaticConcatenatedStrings<3, uint32_t> _parts;
};

}  // namespace aeronet