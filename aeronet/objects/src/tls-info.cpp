#include "tls-info.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "raw-chars.hpp"

namespace aeronet {

TLSInfo::TLSInfo(std::string_view selectedAlpn, std::string_view negotiatedCipher, std::string_view negotiatedVersion)
    : _buf(selectedAlpn.size() + negotiatedCipher.size() + negotiatedVersion.size()),
      _negotiatedCipherBeg(static_cast<uint32_t>(selectedAlpn.size())),
      _negotiatedVersionBeg(static_cast<uint32_t>(selectedAlpn.size() + negotiatedCipher.size())) {
  if (_buf.capacity() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
    throw std::length_error("TLSInfo: strings too large");
  }
  _buf.unchecked_append(selectedAlpn);
  _buf.unchecked_append(negotiatedCipher);
  _buf.unchecked_append(negotiatedVersion);
}

}  // namespace aeronet