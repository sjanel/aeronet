#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "fixedcapacityvector.hpp"

namespace aeronet {

class EncodingSelector {
 public:
  explicit EncodingSelector(const CompressionConfig &compressionConfig);

  // Parse an Accept-Encoding header per RFC 9110 section 12.5.3 and select the
  // best supported encoding among supported ones.
  // Rules implemented:
  //  - Split on commas; each token may have optional parameters separated by ';'
  //  - Extract q parameter (q=0..1, default 1.0). Invalid q -> treated as 0.
  //  - Case-insensitive exact token matching.
  //  - Ignore encodings with q=0.
  //  - Prefer highest q; tie -> server preference (based on ordered values 'preferredFormats')
  //  - Wildcard '*' is supported: applies its q to any supported encoding not explicitly listed (unless that encoding
  //  appeared with q=0).
  //  - If nothing acceptable remains, fall back to identity.
  //  - identity explicitly disallowed (identity;q=0 and all others q=0) -> still returns identity for now (could be 406
  //  later).
  [[nodiscard]] Encoding negotiateAcceptEncoding(std::string_view acceptEncoding) const;

 private:
  FixedCapacityVector<Encoding, kNbContentEncodings> _preferenceOrdered;  // final ordered list
  // Build server preference ordering: if preferredFormats provided (non-empty) we use that
  // sequence first (deduplicated, valid encodings only) followed by any remaining supported
  // encodings not explicitly listed. Otherwise fall back to the static enumeration order.
  int8_t _serverPrefIndex[kNbContentEncodings];
};

}  // namespace aeronet
