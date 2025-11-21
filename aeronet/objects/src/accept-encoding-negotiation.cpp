#include "aeronet/accept-encoding-negotiation.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>
#include <ranges>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/features.hpp"
#include "aeronet/fixedcapacityvector.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/string-equal-ignore-case.hpp"

namespace aeronet {
namespace {

constexpr std::string_view kWhitespace = " \t";

constexpr bool IsEncodingEnabled(Encoding enc) {
  switch (enc) {
    case Encoding::br:
      return brotliEnabled();
    case Encoding::zstd:
      return zstdEnabled();
    case Encoding::gzip:
      [[fallthrough]];
    case Encoding::deflate:
      return zlibEnabled();
    case Encoding::none:
      return true;
    default:
      std::unreachable();
  }
}

constexpr std::string_view trim(std::string_view sv) {
  while (!sv.empty() && kWhitespace.find(sv.front()) != std::string_view::npos) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && kWhitespace.find(sv.back()) != std::string_view::npos) {
    sv.remove_suffix(1);
  }
  return sv;
}

// Parse q-value within a token (portion including parameters); never throws.
double parseQ(std::string_view token) {
  auto scPos = token.find(';');
  if (scPos == std::string_view::npos) {
    return 1.0;
  }
  std::string_view params = token.substr(scPos + 1);
  while (!params.empty()) {
    params = trim(params);
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

// Compile-time construction of supported encodings array from reflected keys
struct Sup {
  std::string_view name;
  Encoding enc;
};

template <std::size_t... I>
consteval auto makeSupported(std::index_sequence<I...> /*unused*/) {
  return std::array<Sup, sizeof...(I)>{{{GetEncodingStr(static_cast<Encoding>(I)), static_cast<Encoding>(I)}...}};
}

}  // namespace

EncodingSelector::EncodingSelector() noexcept { initDefault(); }

void EncodingSelector::initDefault() noexcept {
  std::ranges::iota(_serverPrefIndex, 0);
  for (std::underlying_type_t<Encoding> pos = 0; pos < kNbContentEncodings; ++pos) {
    auto enc = static_cast<Encoding>(pos);
    if (IsEncodingEnabled(enc)) {
      _preferenceOrdered.push_back(enc);
    }
  }
}

EncodingSelector::EncodingSelector(const CompressionConfig &compressionConfig) {
  if (compressionConfig.preferredFormats.empty()) {
    initDefault();
  } else {
    std::ranges::fill(_serverPrefIndex, -1);
    int8_t next = 0;
    for (Encoding enc : compressionConfig.preferredFormats) {
      if (!IsEncodingEnabled(enc)) {
        continue;
      }
      auto idx = static_cast<std::underlying_type_t<Encoding>>(enc);
      if (_serverPrefIndex[idx] == -1) {  // dedupe
        _serverPrefIndex[idx] = next;
        ++next;
        _preferenceOrdered.push_back(enc);
      }
    }
    // Do NOT append remaining encodings: preferredFormats defines the full server-advertised order.
  }
}

EncodingSelector::NegotiatedResult EncodingSelector::negotiateAcceptEncoding(std::string_view acceptEncoding) const {
  NegotiatedResult ret;
  // Fast path: empty or all whitespace -> identity (Encoding::none maps to identity header)
  if (acceptEncoding.empty()) {
    return ret;
  }

  static constexpr auto kSupportedEncodings = makeSupported(std::make_index_sequence<kNbContentEncodings>{});

  struct ParsedToken {
    std::string_view name;
    double quality{1.0};
  };

  FixedCapacityVector<ParsedToken, kNbContentEncodings> knownEncodings;  // only supported encodings captured
  bool sawWildcard = false;
  double wildcardQ = 0.0;

  using SeenBmp = uint8_t;  // accommodate additional encodings (gzip, deflate, zstd, none)

  static_assert(sizeof(SeenBmp) * CHAR_BIT >= kNbContentEncodings);

  SeenBmp seenMask = 0;  // bit i set => supported[i] already stored

  bool identityExplicit = false;
  double identityQ = 0.0;
  for (auto part : acceptEncoding | std::views::split(',')) {
    std::string_view raw{&*part.begin(), static_cast<std::size_t>(std::ranges::distance(part))};
    raw = trim(raw);
    if (raw.empty()) {
      continue;
    }
    auto sc = raw.find(';');
    std::string_view name = trim(sc == std::string_view::npos ? raw : raw.substr(0, sc));
    double quality = parseQ(raw);
    if (CaseInsensitiveEqual(name, "*")) {
      sawWildcard = true;
      wildcardQ = quality;
      continue;
    }
    for (std::size_t pos = 0; pos < kSupportedEncodings.size(); ++pos) {
      if ((seenMask & (1 << pos)) != 0) {
        continue;  // already captured earliest occurrence
      }
      if (CaseInsensitiveEqual(name, kSupportedEncodings[pos].name) &&
          IsEncodingEnabled(kSupportedEncodings[pos].enc)) {
        knownEncodings.emplace_back(kSupportedEncodings[pos].name, quality);
        seenMask |= static_cast<SeenBmp>(1 << pos);
        break;
      }
    }
    if (CaseInsensitiveEqual(name, http::identity)) {
      identityExplicit = true;
      identityQ = quality;  // capture last; earliest identity suffices but we update for clarity
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
  for (const auto &pt : knownEncodings) {
    for (const auto &kSupportedEncoding : kSupportedEncodings) {
      if (CaseInsensitiveEqual(pt.name, kSupportedEncoding.name) && IsEncodingEnabled(kSupportedEncoding.enc)) {
        auto idx = static_cast<int>(kSupportedEncoding.enc);
        consider(kSupportedEncoding.enc, pt.quality, _serverPrefIndex[idx]);
        break;
      }
    }
  }

  // Apply wildcard to any supported encoding not explicitly mentioned, iterating in server preference order.
  if (sawWildcard) {
    for (Encoding enc : _preferenceOrdered) {
      bool mentioned = false;
      for (const auto &pt : knownEncodings) {
        if (CaseInsensitiveEqual(pt.name, GetEncodingStr(enc))) {
          mentioned = true;
          break;
        }
      }
      if (!mentioned) {
        consider(enc, wildcardQ, _serverPrefIndex[static_cast<int>(enc)]);
      }
    }
  }

  if (bestQ < 0.0) {
    // No acceptable compression encodings selected.
    ret.encoding = Encoding::none;
    if (identityExplicit && identityQ <= 0.0) {
      // Client explicitly forbids identity and offered no acceptable alternative.
      ret.reject = true;
    }
  }
  ret.encoding = chosen;
  return ret;
}

}  // namespace aeronet
