#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/fixedcapacityvector.hpp"

namespace aeronet {

class EncodingSelector {
 public:
  EncodingSelector() noexcept;

  explicit EncodingSelector(const CompressionConfig &compressionConfig);

  struct NegotiatedResult {
    Encoding encoding{Encoding::none};

    // Returns true when the
    // client explicitly disallowed identity (identity;q=0) and no other acceptable encodings were present.
    // Call immediately after negotiateAcceptEncoding(); only then is the value meaningful for that request.
    bool reject{false};
  };

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
  //  - If nothing acceptable remains, fall back to identity (Encoding::none) UNLESS client explicitly disallows
  //    identity via identity;q=0 and no other encodings are acceptable (all q=0). In that case we conceptually
  //    signal "no acceptable encoding"; the caller may translate this to 406 Not Acceptable. We surface this by
  //    returning Encoding::invalid (value outside supported range) which callers must check.
  //    (Current implementation: Encoding enum has only valid encodings; we piggy-back by returning none when allowed
  //    and triggering a separate flag for 406 in the parsing logic.)
  // Returns a NegotiatedResult object
  [[nodiscard]] NegotiatedResult negotiateAcceptEncoding(std::string_view acceptEncoding) const;

 private:
  void initDefault() noexcept;

  // final ordered list
  FixedCapacityVector<Encoding, kNbContentEncodings, amc::vec::UncheckedGrowingPolicy> _preferenceOrdered;
  // Build server preference ordering: if preferredFormats provided (non-empty) we use that
  // sequence first (deduplicated, valid encodings only) followed by any remaining supported
  // encodings not explicitly listed. Otherwise fall back to the static enumeration order.
  int8_t _serverPrefIndex[kNbContentEncodings];
};

}  // namespace aeronet
