#include "aeronet/accept-encoding-negotiation.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/fixedcapacityvector.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"

namespace aeronet {
namespace {

using EncodingInt = std::underlying_type_t<Encoding>;

constexpr std::string_view kWhitespace = " \t";

// Parse q-value within a token (portion including parameters); never throws.
double ParseQ(std::string_view token) {
  auto scPos = token.find(';');
  if (scPos == std::string_view::npos) {
    return 1.0;
  }
  std::string_view params = token.substr(scPos + 1);
  while (!params.empty()) {
    params = TrimOws(params);
    auto nextSemi = params.find(';');
    std::string_view param = nextSemi == std::string_view::npos ? params : params.substr(0, nextSemi);
    if (nextSemi == std::string_view::npos) {
      params = {};
    } else {
      params.remove_prefix(nextSemi + 1);
    }
    if (param.size() >= 2 && (param[0] == 'q' || param[0] == 'Q') && param[1] == '=') {
      auto val = param.substr(2);
      while (!val.empty() && kWhitespace.find(val.back()) != std::string_view::npos) {
        val.remove_suffix(1);
      }
      auto cut = val.find_first_of(" ;\t");
      if (cut != std::string_view::npos) {
        val = val.substr(0, cut);
      }
      if (val.empty()) {
        return 0.0;
      }
      double qualityValue = 0.0;
      const char *begin = val.data();
      const char *end = begin + val.size();
      auto fcRes = std::from_chars(begin, end, qualityValue);
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

constexpr std::array<Encoding, kNbContentEncodings> kPreferredEncodingsDefault = [] {
  std::array<Encoding, kNbContentEncodings> arr{};
  EncodingInt sz = 0;
  for (EncodingInt pos = 0; pos < kNbContentEncodings; ++pos) {
    auto enc = static_cast<Encoding>(pos);
    if (IsEncodingEnabled(enc)) {
      arr[sz++] = enc;
    }
  }
  return arr;
}();

constexpr EncodingInt kNbSupportedEncodings = [] {
  EncodingInt count = 0;
  for (EncodingInt pos = 0; pos < kNbContentEncodings; ++pos) {
    if (IsEncodingEnabled(static_cast<Encoding>(pos))) {
      ++count;
    }
  }
  return count;
}();

// Compile-time construction of supported encodings array from reflected keys
struct Sup {
  std::string_view name;
  Encoding enc;
};

constexpr auto kSupportedEncodings = []() {
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

}  // namespace

EncodingSelector::EncodingSelector() noexcept { initDefault(); }

void EncodingSelector::initDefault() noexcept {
  std::ranges::iota(_serverPrefIndex, 0);
  _preferenceOrdered = kPreferredEncodingsDefault;
  _nbPreferences = kNbSupportedEncodings;
}

EncodingSelector::EncodingSelector(const CompressionConfig &compressionConfig) {
  if (compressionConfig.preferredFormats.empty()) {
    initDefault();
  } else {
    int8_t next = 0;
    _nbPreferences = 0;
    // preferredFormats should not contain duplicates
    assert(std::ranges::all_of(compressionConfig.preferredFormats, [&compressionConfig](Encoding enc) {
      return std::ranges::count(compressionConfig.preferredFormats, enc) == 1;
    }));

    for (Encoding enc : compressionConfig.preferredFormats) {
      assert(IsEncodingEnabled(enc));  // config should have been validated
      auto idx = static_cast<EncodingInt>(enc);
      _serverPrefIndex[idx] = next;
      ++next;
      _preferenceOrdered[_nbPreferences++] = enc;
    }
  }
  // Do NOT append remaining encodings: preferredFormats defines the full server-advertised order.
}

EncodingSelector::NegotiatedResult EncodingSelector::negotiateAcceptEncoding(std::string_view acceptEncoding) const {
  NegotiatedResult ret;
  // Fast path: empty or all whitespace -> identity (Encoding::none maps to identity header)
  if (acceptEncoding.empty()) {
    return ret;
  }

  std::array<double, kNbSupportedEncodings> knownEncodingsQuality;  // per supported encoding, highest q
  knownEncodingsQuality.fill(-1.0);
  bool sawWildcard = false;
  double wildcardQ = 0.0;

  bool identityExplicit = false;
  for (http::HeaderValueReverseTokensIterator<','> it(acceptEncoding); it.hasNext();) {
    std::string_view raw = it.next();
    if (raw.empty()) {
      continue;
    }
    auto sc = raw.find(';');
    std::string_view name = TrimOws(sc == std::string_view::npos ? raw : raw.substr(0, sc));
    double quality = ParseQ(raw);
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

  auto consider = [&](Encoding enc, double quality, int serverPreferenceIndex) {
    if (quality <= 0.0) {
      return;  // q=0 means "not acceptable"
    }
    if (quality > bestQ || (quality == bestQ && serverPreferenceIndex < bestServerPreferenceIndex)) {
      bestQ = quality;
      bestServerPreferenceIndex = serverPreferenceIndex;
      chosen = enc;
    }
  };

  // Evaluate explicitly listed supported encodings. We map back to server preference index (via _serverPrefIndex)
  // and pick highest q; ties resolved by lower preference index instead of client header order.
  for (std::size_t pos = 0; pos < kSupportedEncodings.size(); ++pos) {
    const double quality = knownEncodingsQuality[pos];
    if (quality < 0.0) {
      continue;
    }
    const auto enc = kSupportedEncodings[pos].enc;
    consider(enc, quality, _serverPrefIndex[static_cast<EncodingInt>(enc)]);
  }

  // Apply wildcard to any supported encoding not explicitly mentioned, iterating in server preference order.
  if (sawWildcard) {
    for (EncodingInt pos = 0; pos < _nbPreferences; ++pos) {
      const Encoding enc = _preferenceOrdered[pos];

      const auto encPos = std::ranges::find_if(kSupportedEncodings, [enc](const Sup &sup) { return sup.enc == enc; });
      const auto idx = static_cast<std::size_t>(std::distance(kSupportedEncodings.begin(), encPos));
      assert(idx < kSupportedEncodings.size());
      if (knownEncodingsQuality[idx] < 0.0) {
        consider(enc, wildcardQ, _serverPrefIndex[static_cast<EncodingInt>(enc)]);
      }
    }
  }

  if (bestQ < 0.0) {
    // No acceptable compression encodings selected.
    ret.encoding = Encoding::none;
    if (identityExplicit) {
      // Client explicitly forbids identity and offered no acceptable alternative.
      ret.reject = true;
    }
  }
  ret.encoding = chosen;
  return ret;
}

}  // namespace aeronet
