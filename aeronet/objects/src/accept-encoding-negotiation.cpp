#include "aeronet/accept-encoding-negotiation.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string_view>
#include <system_error>
#include <type_traits>

// std::from_chars for floating-point types is standard since C++17, but libc++ (Apple's default
// standard library) ships to_chars(double) yet still lacks from_chars(double). Detect the real
// capability via the <charconv> feature-test macro instead of hard-coding the OS, so any toolchain
// shipping a complete <charconv> takes the fast from_chars path and only the rest falls back to strtod.
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
#define AERONET_HAS_STD_FLOAT_FROM_CHARS 1
#else
#define AERONET_HAS_STD_FLOAT_FROM_CHARS 0
#include <cstdlib>  // std::strtod
#include <cstring>  // std::memcpy
#endif

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"

namespace aeronet {
namespace {

using EncodingInt = std::underlying_type_t<Encoding>;

constexpr std::string_view kWhitespace = " \t";

// Parse q-value within a token (portion including parameters); never throws.
double ParseQ(std::string_view token) {
  const auto scPos = token.find(';');
  if (scPos == std::string_view::npos) {
    return 1.0;
  }
  std::string_view params = token.substr(scPos + 1);
  while (!params.empty()) {
    params = TrimOws(params);
    const auto nextSemi = params.find(';');
    const std::string_view param = nextSemi == std::string_view::npos ? params : params.substr(0, nextSemi);
    if (nextSemi == std::string_view::npos) {
      params = {};
    } else {
      params.remove_prefix(nextSemi + 1);
    }
    if (param.size() >= 2 && (param[0] == 'q' || param[0] == 'Q') && param[1] == '=') {
      auto val = param.substr(2);
      while (!val.empty() && kWhitespace.contains(val.back())) {
        val.remove_suffix(1);
      }
      const auto cut = val.find_first_of(" ;\t");
      if (cut != std::string_view::npos) {
        val = val.substr(0, cut);
      }
      if (val.empty()) {
        return 0.0;
      }
      double qualityValue = 0.0;
      const char* begin = val.data();
      const char* end = begin + val.size();
#if AERONET_HAS_STD_FLOAT_FROM_CHARS
      const auto fcRes = std::from_chars(begin, end, qualityValue);
#else
      // No from_chars(double) (see feature-test guard above): copy into a NUL-terminated stack buffer
      // and use strtod, reconstructing a from_chars_result-like {ptr, ec} so the validation below is
      // identical on both paths.
      char buf[64];
      const auto len = std::min(val.size(), static_cast<std::string_view::size_type>(sizeof(buf) - 1));
      std::memcpy(buf, begin, len);
      buf[len] = '\0';
      char* endPtr = nullptr;
      qualityValue = std::strtod(buf, &endPtr);
      struct {
        const char* ptr;
        std::errc ec;
      } fcRes{begin + (endPtr - buf), (endPtr != buf) ? std::errc{} : std::errc::invalid_argument};
#endif
      if (fcRes.ec != std::errc() || fcRes.ptr != end) {
        return 0.0;  // invalid format
      }
      if (qualityValue < 0.0) {
        qualityValue = 0.0;
      } else if (qualityValue > 1.0) {
        qualityValue = 1.0;
      }
      return qualityValue;
    }
  }
  return 1.0;
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
    auto enc = static_cast<Encoding>(pos);
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

  double knownEncodingsQuality[kNbSupportedEncodings];  // per supported encoding, highest q
  std::ranges::fill(knownEncodingsQuality, -1.0);
  bool sawWildcard = false;
  double wildcardQ = 0.0;

  bool identityExplicit = false;
  for (http::HeaderValueReverseTokensIterator<','> it(acceptEncoding); it.hasNext();) {
    const std::string_view raw = it.next();
    if (raw.empty()) {
      continue;
    }
    const auto sc = raw.find(';');
    const std::string_view name = TrimOws(sc == std::string_view::npos ? raw : raw.substr(0, sc));
    const double quality = ParseQ(raw);
    if (name == "*") {
      sawWildcard = true;
      wildcardQ = quality;
      continue;
    }
    for (std::size_t pos = 0; pos < kSupportedEncodings.size(); ++pos) {
      assert(IsEncodingEnabled(kSupportedEncodings[pos].enc));
      if (CaseInsensitiveEqual(name, kSupportedEncodings[pos].name)) {
        knownEncodingsQuality[pos] = std::max(quality, knownEncodingsQuality[pos]);
        break;
      }
    }
    if (CaseInsensitiveEqual(name, http::identity)) {
      identityExplicit = true;
    }
  }

  Encoding chosen = Encoding::none;
  double bestQ = -1.0;
  int bestServerPreferenceIndex = std::numeric_limits<int>::max();

  // Pick the highest q among the explicitly listed supported encodings and, with a wildcard, the unlisted ones. Ties
  // are resolved by the lower server preference rank instead of client header order. Ranks being distinct, the result
  // does not depend on the order in which encodings are evaluated.
  for (std::size_t pos = 0; pos < kSupportedEncodings.size(); ++pos) {
    double quality = knownEncodingsQuality[pos];
    if (quality < 0.0) {
      if (!sawWildcard) {
        continue;
      }
      quality = wildcardQ;  // the wildcard applies to any supported encoding not explicitly listed
    }
    if (quality <= 0.0) {
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
  ret.reject = bestQ < 0.0 && (identityExplicit || sawWildcard);
  ret.encoding = chosen;
  return ret;
}

}  // namespace aeronet
