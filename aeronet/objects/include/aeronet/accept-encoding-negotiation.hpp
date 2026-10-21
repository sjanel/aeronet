#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"

namespace aeronet {

class EncodingSelector {
 public:
  EncodingSelector() noexcept;

  explicit EncodingSelector(const CompressionConfig& compressionConfig) noexcept;

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
  // The result for the last value of at most kMaxCachedAcceptEncodingSize bytes is remembered: the clients of a server
  // mostly send the same value (that of their browser or HTTP library), whose negotiation is then a comparison.
  // Returns a NegotiatedResult object
  [[nodiscard]] NegotiatedResult negotiateAcceptEncoding(std::string_view acceptEncoding);

  // Longest Accept-Encoding value whose result is remembered: that of the current browsers, the longest common one
  // (HTTP libraries send shorter ones, or the same codings in another order).
  static constexpr std::size_t kMaxCachedAcceptEncodingSize = std::string_view("gzip, deflate, br, zstd").size();

 private:
  // Negotiates a non-empty Accept-Encoding value, without the cache.
  [[nodiscard]] NegotiatedResult negotiate(std::string_view acceptEncoding) const;

  // Server preference order of the supported encodings: the 'preferredFormats' sequence first (when provided),
  // followed by the remaining supported encodings in the static enumeration order.
  Encoding _preferenceOrdered[kNbContentEncodings];
  // Position of each supported encoding in _preferenceOrdered (lower is preferred).
  int8_t _serverPrefIndex[kNbContentEncodings];
  // The last negotiated value (if not longer than kMaxCachedAcceptEncodingSize) and its result. A size of 0 means no
  // value, an empty one being negotiated without the cache.
  NegotiatedResult _cachedResult;
  uint8_t _cachedAcceptEncodingSize{0};
  char _cachedAcceptEncoding[kMaxCachedAcceptEncodingSize];
};

}  // namespace aeronet
