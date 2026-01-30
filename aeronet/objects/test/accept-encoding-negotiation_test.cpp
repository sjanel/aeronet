#include "aeronet/accept-encoding-negotiation.hpp"

#include <gtest/gtest.h>

#include <initializer_list>  // std::initializer_list
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/features.hpp"

namespace aeronet {

namespace {
EncodingSelector MakeSelector(std::initializer_list<Encoding> prefs) {
  CompressionConfig cfg;
  for (auto enc : prefs) {
    cfg.preferredFormats.push_back(enc);
  }
  cfg.validate();
  return EncodingSelector(cfg);
}

constexpr auto kDefaultEncodingGzipEnabled = aeronet::zlibEnabled() ? aeronet::Encoding::gzip : aeronet::Encoding::none;
constexpr auto kDefaultEncodingDeflateEnabled =
    aeronet::zlibEnabled() ? aeronet::Encoding::deflate : aeronet::Encoding::none;

}  // namespace

TEST(AcceptEncodingNegotiationTest, EmptyOrWhitespace) {
  CompressionConfig cfg;  // no preferred formats -> all default enumeration order
  EncodingSelector sel(cfg);
  {
    auto resultEmpty = sel.negotiateAcceptEncoding("");
    EXPECT_EQ(resultEmpty.encoding, Encoding::none);
    EXPECT_FALSE(resultEmpty.reject);
  }
  {
    auto resultWs = sel.negotiateAcceptEncoding("   \t");
    EXPECT_EQ(resultWs.encoding, Encoding::none);
    EXPECT_FALSE(resultWs.reject);
  }
}

TEST(AcceptEncodingNegotiationTest, IgnoreUnsupportedWithWildcard) {
  CompressionConfig cfg;
  if constexpr (aeronet::zlibEnabled()) {
    cfg.preferredFormats.push_back(Encoding::gzip);
    cfg.preferredFormats.push_back(Encoding::deflate);
  }
  auto sel = EncodingSelector(cfg);
  if constexpr (aeronet::zlibEnabled()) {
    EXPECT_EQ(sel.negotiateAcceptEncoding("snappy;q=0.9, *;q=0.5").encoding, Encoding::gzip);
  } else if constexpr (aeronet::zstdEnabled()) {
    EXPECT_EQ(sel.negotiateAcceptEncoding("snappy;q=0.9, *;q=0.5").encoding, Encoding::zstd);
  } else if constexpr (aeronet::brotliEnabled()) {
    EXPECT_EQ(sel.negotiateAcceptEncoding("snappy;q=0.9, *;q=0.5").encoding, Encoding::br);
  }
}

TEST(AcceptEncodingNegotiationTest, InvalidQValues) {
  if constexpr (aeronet::zlibEnabled()) {
    auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
    EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=abc, deflate").encoding,
              Encoding::deflate);  // invalid q for gzip treated as 0
  } else {
    EncodingSelector sel(CompressionConfig{});
    // gzip unsupported -> ignored -> identity
    EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=abc").encoding, Encoding::none);
  }
}

TEST(AcceptEncodingNegotiationTest, SpacesAndTabs) {
  if constexpr (aeronet::zlibEnabled()) {
    auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
    EXPECT_EQ(sel.negotiateAcceptEncoding(" gzip ; q=1 , deflate ; q=0.4").encoding, Encoding::gzip);
  } else {
    EncodingSelector sel(CompressionConfig{});
    EXPECT_EQ(sel.negotiateAcceptEncoding(" gzip ; q=1 ").encoding, Encoding::none);
  }
}

// Additional comprehensive scenarios covering tie-breaks, wildcard, duplicates, and preference nuances.

TEST(AcceptEncodingNegotiationTest, TieBreakNoPreferencesUsesEnumOrder) {
  CompressionConfig cfg;  // no preferredFormats -> enum order among enabled encodings
  EncodingSelector sel(cfg);
  if constexpr (aeronet::zlibEnabled()) {
    EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.7, deflate;q=0.7").encoding, Encoding::gzip);
  } else {
    // With no compression codecs enabled, both tokens unsupported -> identity
    EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.7, deflate;q=0.7").encoding, Encoding::none);
  }
}

TEST(AcceptEncodingNegotiationTest, WildcardZeroPreventsUnlistedSelection) {
  CompressionConfig cfg;
  if constexpr (aeronet::zlibEnabled()) {
    cfg.preferredFormats.push_back(Encoding::gzip);
    cfg.preferredFormats.push_back(Encoding::deflate);
  }
  auto sel = EncodingSelector(cfg);
  // Explicit gzip q=0 -> not acceptable; wildcard q=0 -> no others acceptable -> identity (none)
  {
    auto resultWildcardAllZero = sel.negotiateAcceptEncoding("gzip;q=0, *;q=0");
    EXPECT_EQ(resultWildcardAllZero.encoding, Encoding::none);
    EXPECT_FALSE(resultWildcardAllZero.reject);
  }
}

#ifdef AERONET_ENABLE_ZLIB
TEST(AcceptEncodingNegotiationTest, SimpleExactMatches) {
  CompressionConfig cfg;
  cfg.preferredFormats.push_back(Encoding::gzip);
  cfg.preferredFormats.push_back(Encoding::deflate);
  if constexpr (aeronet::zstdEnabled()) {
    cfg.preferredFormats.push_back(Encoding::zstd);
  }
  if constexpr (aeronet::brotliEnabled()) {
    cfg.preferredFormats.push_back(Encoding::br);
  }
  auto sel = EncodingSelector(cfg);
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip").encoding, Encoding::gzip);
  EXPECT_EQ(sel.negotiateAcceptEncoding("deflate").encoding, Encoding::deflate);
  if constexpr (aeronet::zstdEnabled()) {
    EXPECT_EQ(sel.negotiateAcceptEncoding("zstd").encoding, Encoding::zstd);
  }
  if constexpr (aeronet::brotliEnabled()) {
    EXPECT_EQ(sel.negotiateAcceptEncoding("br").encoding, Encoding::br);
  }
}

TEST(AcceptEncodingNegotiationTest, CaseInsensitive) {
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("GZIP").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, WithParametersOrderAndQ) {
  // Prefer higher q even if later
  CompressionConfig cfg;
  cfg.preferredFormats.push_back(Encoding::gzip);
  cfg.preferredFormats.push_back(Encoding::deflate);
  if constexpr (aeronet::zstdEnabled()) {
    cfg.preferredFormats.push_back(Encoding::zstd);
  }
  if constexpr (aeronet::brotliEnabled()) {
    cfg.preferredFormats.push_back(Encoding::br);
  }
  auto sel = EncodingSelector(cfg);
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.5, deflate;q=0.9").encoding, Encoding::deflate);
  // Tie in q -> server preference order (gzip preferred over deflate when equal q with current prefs)
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.8, deflate;q=0.8").encoding, Encoding::gzip);
  if constexpr (aeronet::zstdEnabled()) {
    // Higher q for zstd should select it
    EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.8, deflate;q=0.8, zstd;q=0.95").encoding, Encoding::zstd);
  }
  if constexpr (aeronet::brotliEnabled()) {
    EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.8, deflate;q=0.8, br;q=0.96").encoding, Encoding::br);
  }
}

TEST(AcceptEncodingNegotiationTest, IdentityFallback) {
  // All unsupported or q=0 -> identity. Use a definitely unsupported token sequence.
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  if constexpr (aeronet::brotliEnabled()) {
    EXPECT_EQ(sel.negotiateAcceptEncoding("snappy, lz4").encoding, Encoding::none);
  } else {
    EXPECT_EQ(sel.negotiateAcceptEncoding("br, lz4").encoding, Encoding::none);
  }
  {
    auto resultAllZero = sel.negotiateAcceptEncoding("gzip;q=0, deflate;q=0");
    EXPECT_EQ(resultAllZero.encoding, Encoding::none);
    EXPECT_FALSE(resultAllZero.reject) << "Identity not explicitly forbidden so reject flag must be false";
  }
}

TEST(AcceptEncodingNegotiationTest, Wildcard) {
  // Wildcard picks first server preference not explicitly mentioned
  auto sel = MakeSelector({Encoding::deflate, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("*;q=0.9").encoding, Encoding::deflate);
  // Explicit better q wins
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.4, *;q=0.3").encoding, Encoding::gzip);
  // Wildcard lower q than explicit
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.9, *;q=0.1").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, TieBreakWithReversedPreferencesPicksDeflate) {
  auto sel = MakeSelector({Encoding::deflate, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.7, deflate;q=0.7").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, HigherQOverridesPreferenceList) {
  auto sel = MakeSelector({Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.9, deflate;q=0.8").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, HigherQForPreferredBeatsUnlisted) {
  auto sel = MakeSelector({Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.6, deflate;q=0.5").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, WildcardSelectsFirstPreferenceWhenNoExplicit) {
  auto sel = MakeSelector({Encoding::deflate, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("*;q=0.8").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, WildcardDoesNotOverrideBetterExplicitQ) {
  auto sel = MakeSelector({Encoding::deflate, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.9, *;q=0.5").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, WildcardFillsForUnlistedWhenPositive) {
  CompressionConfig cfg;
  cfg.preferredFormats.push_back(Encoding::gzip);
  auto sel = EncodingSelector(cfg);
  EXPECT_EQ(sel.negotiateAcceptEncoding("*;q=0.6, gzip;q=0.6").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, ExplicitQZeroSkipsEncoding) {
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0, deflate;q=0.5").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, SameKeyMultipleEntriesTakesHighestQ1) {
  auto sel = MakeSelector({Encoding::deflate, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.2, gzip;q=0.9, deflate;q=0.2").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, SameKeyMultipleEntriesTakesHighestQ2) {
  auto sel = MakeSelector({Encoding::deflate, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.9, gzip;q=0.1, deflate;q=0.2").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, DuplicateWithLowerQLaterDoesNotChangeChoice) {
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("deflate;q=0.8, deflate;q=0.1, gzip;q=0.9").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, InvalidQParsesAsZero) {
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=abc, deflate;q=0.4").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, QGreaterThanOneClamped) {
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("deflate;q=5, gzip;q=0.9").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, NegativeQClampedToZero) {
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=-1, deflate;q=0.3").encoding, Encoding::deflate);
}
#endif

TEST(AcceptEncodingNegotiationTest, IdentityExplicitHigherQChosenIfHigherQ) {
#ifdef AERONET_ENABLE_ZLIB
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  auto resultIdentityHigh = sel.negotiateAcceptEncoding("identity;q=1, gzip;q=0.8");
#else
  EncodingSelector sel(CompressionConfig{});
  auto resultIdentityHigh = sel.negotiateAcceptEncoding("identity;q=1, gzip;q=0.8");  // gzip ignored
#endif
  EXPECT_EQ(resultIdentityHigh.encoding, Encoding::none);
  EXPECT_FALSE(resultIdentityHigh.reject);
}

TEST(AcceptEncodingNegotiationTest, IdentityPreferredInConfigWins) {
  // If the server preference explicitly contains identity (none), it should be
  // selected when q values allow it even if other encodings are present.
  CompressionConfig cfg;
  cfg.preferredFormats.push_back(Encoding::none);
  if constexpr (aeronet::zlibEnabled()) {
    cfg.preferredFormats.push_back(Encoding::gzip);
  }
  cfg.validate();
  auto sel = EncodingSelector(cfg);
  // Client prefers gzip slightly more, so gzip should be chosen despite server listing
  // identity first in its preference list.
  auto res = sel.negotiateAcceptEncoding("gzip;q=0.8, identity;q=0.7");
  EXPECT_EQ(res.encoding, kDefaultEncodingGzipEnabled);
}

TEST(AcceptEncodingNegotiationTest, IdentityPreferredButForbiddenSetsReject) {
  CompressionConfig cfg;
  cfg.preferredFormats.push_back(Encoding::none);
  if constexpr (aeronet::zlibEnabled()) {
    cfg.preferredFormats.push_back(Encoding::gzip);
  }
  cfg.validate();
  auto sel = EncodingSelector(cfg);
  // If identity explicitly has q=0 and no alternatives acceptable, negotiation should set reject
  auto res = sel.negotiateAcceptEncoding("identity;q=0, gzip;q=0");
  EXPECT_EQ(res.encoding, Encoding::none);
  EXPECT_TRUE(res.reject);
}

TEST(AcceptEncodingNegotiationTest, IdentityPreferredAndChosenWhenHigherQ) {
  CompressionConfig cfg;
  cfg.preferredFormats.push_back(Encoding::none);
  if constexpr (aeronet::zlibEnabled()) {
    cfg.preferredFormats.push_back(Encoding::gzip);
  }
  cfg.validate();
  auto sel = EncodingSelector(cfg);
  // If identity has a higher q than gzip, identity should be selected per client preference.
  auto res = sel.negotiateAcceptEncoding("identity;q=0.95, gzip;q=0.8");
  EXPECT_EQ(res.encoding, Encoding::none);
}

TEST(AcceptEncodingNegotiationTest, AllCompressionQZeroFallsBackToIdentity) {
#ifdef AERONET_ENABLE_ZLIB
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  auto resultAllCompressionZero = sel.negotiateAcceptEncoding("gzip;q=0, deflate;q=0");
  EXPECT_EQ(resultAllCompressionZero.encoding, Encoding::none);
  EXPECT_FALSE(resultAllCompressionZero.reject);
#else
  EncodingSelector sel(CompressionConfig{});
  auto res = sel.negotiateAcceptEncoding("gzip;q=0");  // ignored -> identity
  EXPECT_EQ(res.encoding, Encoding::none);
  EXPECT_FALSE(res.reject);
#endif
}

#ifdef AERONET_ENABLE_ZLIB
TEST(AcceptEncodingNegotiationTest, IdentityExplicitButAcceptableWhenAlternativesForbidden) {
  // identity explicitly present with q>0, but all compression encodings have q=0.
  // This should select identity (Encoding::none) and not set reject.
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  auto res = sel.negotiateAcceptEncoding("identity;q=0.5, gzip;q=0, deflate;q=0");
  EXPECT_EQ(res.encoding, Encoding::none);
  EXPECT_FALSE(res.reject);
}
#endif

TEST(AcceptEncodingNegotiationTest, MixedCaseAndSpacesRobust) {
#ifdef AERONET_ENABLE_ZLIB
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("  GzIp ; Q=0.7 ,  DeFlAtE ; q=0.9  ").encoding, Encoding::deflate);
#else
  EncodingSelector sel(CompressionConfig{});
  EXPECT_EQ(sel.negotiateAcceptEncoding("  GzIp ; Q=0.7  ").encoding, Encoding::none);
#endif
}

#ifndef AERONET_ENABLE_ZLIB
TEST(AcceptEncodingNegotiationTest, UnsupportedGzipIgnored) {
  EncodingSelector sel(CompressionConfig{});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip").encoding, Encoding::none);
}
#endif

#ifdef AERONET_ENABLE_ZLIB
TEST(AcceptEncodingNegotiationTest, TrailingCommasAndEmptyTokensIgnored) {
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding(",,,gzip;q=0.4,,deflate;q=0.4,,,").encoding, kDefaultEncodingGzipEnabled);
}

TEST(AcceptEncodingNegotiationTest, IdentityExplicitlyForbiddenAndNoAlternativesSetsReject) {
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  auto resultReject = sel.negotiateAcceptEncoding("identity;q=0, gzip;q=0, deflate;q=0");
  EXPECT_EQ(resultReject.encoding, Encoding::none);
  EXPECT_TRUE(resultReject.reject);
}

TEST(AcceptEncodingNegotiationTest, SingleCharQParameterIsIgnored) {
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  // The parameter list contains a single-character `q` (no '='), which should
  // not be treated as a q-value. ParseQ should therefore return the default
  // quality (1.0) for gzip and gzip should be preferred over deflate.
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q, deflate;q=0.9").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, ParseQEmptyValueTreatedAsZero) {
  // q= with no value should be treated as 0
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  // gzip has an empty q -> treated as 0, deflate should be chosen
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=, deflate;q=0.5").encoding, kDefaultEncodingDeflateEnabled);
}

TEST(AcceptEncodingNegotiationTest, ParseQTrailingWhitespaceAndCut) {
  // whitespace after q value and additional params should be trimmed/cut
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  // gzip's q has trailing space and an extra param after a space which should be ignored
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.85 ;foo=bar, deflate;q=0.8").encoding, kDefaultEncodingGzipEnabled);
}

TEST(AcceptEncodingNegotiationTest, ParseQInvalidFromCharsReturnsZero) {
  // Non-numeric q should parse as 0 (std::from_chars failure)
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});

  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.8a, deflate;q=0.4").encoding, kDefaultEncodingDeflateEnabled);
}

TEST(AcceptEncodingNegotiationTest, ParseQNegativeAndGreaterThanOneClamped) {
  // Negative q clamped to 0, >1 clamped to 1
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  // gzip negative -> 0, deflate >1 -> clamped to 1 -> deflate chosen
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=-0.5, deflate;q=2.0").encoding, kDefaultEncodingDeflateEnabled);
}

TEST(AcceptEncodingNegotiationTest, ParseQCutOnFirstSpaceAfterValue) {
  // Ensure that when q value is followed by a space and additional text (not a semicolon),
  // ParseQ cuts at the first space and uses the numeric portion.
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  // gzip;q=0.77 extra=ignored -> cut should pick "0.77" and gzip selected
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.77 extra=ignored, deflate;q=0.5").encoding,
            kDefaultEncodingGzipEnabled);
}

TEST(AcceptEncodingNegotiationTest, ParseQNoQParamsReturnsDefaultOne) {
  // If parameters exist but none are q=, ParseQ should return 1.0
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  // gzip has a parameter but no q -> treated as q=1.0 -> gzip chosen
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;foo=bar, deflate;q=0.5").encoding, kDefaultEncodingGzipEnabled);
}

#ifdef AERONET_ENABLE_ZSTD
TEST(AcceptEncodingNegotiationTest, ZstdPreferredWhenHighestQ) {
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate, Encoding::zstd});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.7, zstd;q=0.9, deflate;q=0.8").encoding, Encoding::zstd);
}

TEST(AcceptEncodingNegotiationTest, ZstdViaWildcard) {
  auto sel = MakeSelector({Encoding::zstd, Encoding::gzip});
  // zstd not explicitly listed -> wildcard applies; chosen because first preference
  EXPECT_EQ(sel.negotiateAcceptEncoding("*;q=0.5, gzip;q=0.5").encoding, Encoding::zstd);
}

TEST(AcceptEncodingNegotiationTest, WildcardMultiTierZstdDeflateTieBreak) {
  // Server prefers gzip > zstd > deflate (enum order mirrored by explicit list missing gzip)
  auto sel = MakeSelector({Encoding::zstd, Encoding::deflate});
  // Client: wildcard gives q=0.8 to both unlisted (zstd, deflate); explicit deflate at same q as wildcard.
  // Result should pick zstd (higher server preference among tied q encodings that are candidates via wildcard or
  // explicit).
  EXPECT_EQ(sel.negotiateAcceptEncoding("deflate;q=0.8, *;q=0.8").encoding, Encoding::zstd);
  // Now raise deflate q so it wins if zlib is enabled
  if constexpr (aeronet::zlibEnabled()) {
    EXPECT_EQ(sel.negotiateAcceptEncoding("deflate;q=0.9, *;q=0.8").encoding, Encoding::deflate);
  }
}
#endif

#ifdef AERONET_ENABLE_BROTLI
TEST(AcceptEncodingNegotiationTest, BrotliPreferredWhenHighestQ) {
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate, Encoding::br});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.7, br;q=0.95, deflate;q=0.8").encoding, Encoding::br);
}

TEST(AcceptEncodingNegotiationTest, BrotliViaWildcard) {
  auto sel = MakeSelector({Encoding::br, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("*;q=0.5, gzip;q=0.5").encoding, Encoding::br);
}
#endif

TEST(AcceptEncodingNegotiationTest, QCharFollowedByNonEqualsIsIgnored) {
  auto sel = MakeSelector({Encoding::gzip, Encoding::deflate});
  // Parameter begins with 'q' but second char is not '=' (e.g. 'q1=0.5').
  // This should NOT be treated as a q-value; gzip remains at default q=1.0.
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q1=0.5, deflate;q=0.9").encoding, Encoding::gzip);
}
#endif

}  // namespace aeronet