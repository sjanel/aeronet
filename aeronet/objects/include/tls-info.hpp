#pragma once

#include <cstdint>
#include <string_view>

#include "raw-chars.hpp"

namespace aeronet {

// Optimized pack of TLS str information in one allocated storage containing the following information:
// negotiated ALPN protocol (if any)
// negotiated TLS cipher suite (if TLS)
// negotiated TLS protocol version string
class TLSInfo {
 public:
  TLSInfo() noexcept = default;

  TLSInfo(std::string_view selectedAlpn, std::string_view negotiatedCipher, std::string_view negotiatedVersion);

  [[nodiscard]] std::string_view selectedAlpn() const noexcept { return {_buf.data(), _negotiatedCipherBeg}; }

  [[nodiscard]] std::string_view negotiatedCipher() const noexcept {
    return {_buf.data() + _negotiatedCipherBeg, _negotiatedVersionBeg - _negotiatedCipherBeg};
  }

  [[nodiscard]] std::string_view negotiatedVersion() const noexcept {
    return {_buf.data() + _negotiatedVersionBeg, _buf.size() - _negotiatedVersionBeg};
  }

 private:
  RawChars _buf;

  uint32_t _negotiatedCipherBeg{};   // negotiated TLS cipher suite (if TLS)
  uint32_t _negotiatedVersionBeg{};  // negotiated TLS protocol version string
};

}  // namespace aeronet