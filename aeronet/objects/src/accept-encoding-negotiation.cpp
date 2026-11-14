#include "aeronet/accept-encoding-negotiation.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string_view>
#include <type_traits>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/header-value-parameters.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-value-reverse-tokens-iterator.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/quality-value.hpp"
#include "aeronet/string-equal-ignore-case.hpp"

namespace aeronet {
namespace {

using EncodingInt = std::underlying_type_t<Encoding>;

// The weight of an element of Accept-Encoding, from its remaining parameters: its 'q' parameter, 1 by default, or
// http::kInvalidQualityValue.
http::QualityValue ParseQ(http::HeaderParametersIterator& parameters) noexcept {
  while (parameters.hasNext()) {
    const http::HeaderParameter parameter = parameters.next();
    if (parameter.hasValue() && CaseInsensitiveEqual(parameter.name, "q")) {
      return http::TryParseQualityValue(parameter.value);
    }
  }
  return http::kQualityValueMax;
}

// Compile-time construction of supported encodings array from reflected keys, in enumeration order (the default server
// preference order).
struct Sup {
  std::string_view name;
  Encoding enc;
};

constexpr auto kSupportedEncodings = [] {
  std::array<Sup, kNbSupportedEncodings> arr;
  EncodingInt sz = 0;
  for (EncodingInt pos = 0; pos < kNbContentEncodings; ++pos) {
    Encoding enc = static_cast<Encoding>(pos);
    if (IsEncodingEnabled(enc)) {
      arr[sz++] = Sup{GetEncodingStr(enc), enc};
    }
  }
  return arr;
}();

static_assert(kSupportedEncodings.back().enc == Encoding::none);

}  // namespace

EncodingSelector::EncodingSelector() noexcept { std::ranges::iota(_serverPrefIndex, uint8_t{0}); }

EncodingSelector::EncodingSelector(const CompressionConfig& compressionConfig) noexcept {
  const auto& preferredFormats = compressionConfig.preferredFormats;
  // preferredFormats should only contain enabled encodings (config should have been validated), without duplicates
  assert(std::ranges::all_of(preferredFormats, [&preferredFormats](Encoding enc) {
    return IsEncodingEnabled(enc) && std::ranges::count(preferredFormats, enc) == 1;
  }));

  // The configured encodings come first, in their order, followed by the remaining supported encodings in default
  // order, so that every supported encoding has a defined server preference rank.
  auto nextRank = static_cast<uint8_t>(preferredFormats.size());
  for (std::size_t pos = 0; pos < kSupportedEncodings.size(); ++pos) {
    const auto it = std::ranges::find(preferredFormats, kSupportedEncodings[pos].enc);
    _serverPrefIndex[pos] =
        it == preferredFormats.end() ? nextRank++ : static_cast<uint8_t>(it - preferredFormats.begin());
  }
  assert(nextRank == kNbSupportedEncodings);
}

EncodingSelector::NegotiatedResult EncodingSelector::negotiateAcceptEncoding(std::string_view acceptEncoding) {
  // Fast path: empty -> identity (Encoding::none maps to identity header)
  if (acceptEncoding.empty()) {
    return {};
  }
  if (acceptEncoding == std::string_view(_cachedAcceptEncoding, _cachedAcceptEncodingSize)) {
    return _cachedResult;
  }
  const NegotiatedResult result = negotiate(acceptEncoding);
  if (acceptEncoding.size() <= kMaxCachedAcceptEncodingSize) {
    Copy(acceptEncoding, _cachedAcceptEncoding);
    _cachedAcceptEncodingSize = static_cast<uint8_t>(acceptEncoding.size());
    _cachedResult = result;
  }
  return result;
}

EncodingSelector::NegotiatedResult EncodingSelector::negotiate(std::string_view acceptEncoding) const {
  assert(!acceptEncoding.empty());
  NegotiatedResult ret;

  static_assert(kNbSupportedEncodings <= 32U, "one bit of 'listedEncodings' per supported encoding");
  http::QualityValue knownEncodingsQuality[kNbSupportedEncodings]{};  // per listed supported encoding, highest q
  std::uint32_t listedEncodings = 0;                                  // bit 'pos' for kSupportedEncodings[pos]
  bool sawWildcard = false;
  http::QualityValue wildcardQ = 0;

  bool identityExplicit = false;
  for (http::HeaderValueReverseTokensIterator it(acceptEncoding); it.hasNext();) {
    const std::string_view element = it.next();
    if (element.empty()) {
      continue;
    }
    http::HeaderParametersIterator parameters(element);
    const std::string_view name = parameters.firstElement();
    http::QualityValue quality = ParseQ(parameters);
    if (quality == http::kInvalidQualityValue) {
      // An invalid weight counts as 0 for a coding, which is then not selected. For identity or the wildcard, it would
      // make the identity unacceptable, and the request answered 406, while RFC 9110 §12.5.3 only refuses when it is
      // "indicated as unacceptable": their element is ignored.
      if (name == "*" || CaseInsensitiveEqual(name, http::identity)) {
        continue;
      }
      quality = 0;
    }
    if (name == "*") {
      sawWildcard = true;
      wildcardQ = std::max(wildcardQ, quality);
      continue;
    }
    for (std::size_t pos = 0; pos < kSupportedEncodings.size(); ++pos) {
      assert(IsEncodingEnabled(kSupportedEncodings[pos].enc));
      if (CaseInsensitiveEqual(name, kSupportedEncodings[pos].name)) {
        knownEncodingsQuality[pos] = std::max(quality, knownEncodingsQuality[pos]);
        listedEncodings |= 1U << pos;
        break;
      }
    }
    if (CaseInsensitiveEqual(name, http::identity)) {
      identityExplicit = true;
    }
  }

  Encoding chosen = Encoding::none;
  http::QualityValue bestQ = 0;  // 0 while no encoding is acceptable
  int bestServerPreferenceIndex = std::numeric_limits<int>::max();

  // Pick the highest q among the explicitly listed supported encodings and, with a wildcard, the unlisted ones. Ties
  // are resolved by the lower server preference rank instead of client header order. Ranks being distinct, the result
  // does not depend on the order in which encodings are evaluated.
  for (std::size_t pos = 0; pos < kSupportedEncodings.size(); ++pos) {
    http::QualityValue quality = knownEncodingsQuality[pos];
    if ((listedEncodings & (1U << pos)) == 0) {
      if (!sawWildcard) {
        continue;
      }
      quality = wildcardQ;  // the wildcard applies to any supported encoding not explicitly listed
    }
    if (quality == 0) {
      continue;  // q=0 means "not acceptable"
    }
    const int serverPreferenceIndex = _serverPrefIndex[pos];
    if (quality > bestQ || (quality == bestQ && serverPreferenceIndex < bestServerPreferenceIndex)) {
      bestQ = quality;
      bestServerPreferenceIndex = serverPreferenceIndex;
      chosen = kSupportedEncodings[pos].enc;
    }
  }

  // Nothing acceptable, identity included. Identity is acceptable by default, unless excluded by 'identity;q=0' or by
  // '*;q=0' without 'identity' entry (RFC 9110 §12.5.3): a positive q for either would have selected it above.
  ret.reject = bestQ == 0 && (identityExplicit || sawWildcard);
  ret.encoding = chosen;
  return ret;
}

}  // namespace aeronet
