#include "aeronet/accept-encoding-negotiation.hpp"

#include <gtest/gtest.h>

#include <initializer_list>  // std::initializer_list
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/features.hpp"

namespace aeronet {

namespace {
EncodingSelector makeSelector(std::initializer_list<Encoding> prefs) {
  CompressionConfig cfg;
  for (auto enc : prefs) {
    cfg.preferredFormats.push_back(enc);
  }
  return EncodingSelector(cfg);
}
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

TEST(AcceptEncodingNegotiationTest, SimpleExactMatches) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
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
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("GZIP").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, WithParametersOrderAndQ) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  // Prefer higher q even if later
  {
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
}

TEST(AcceptEncodingNegotiationTest, IdentityFallback) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  // All unsupported or q=0 -> identity. Use a definitely unsupported token sequence.
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
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
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  // Wildcard picks first server preference not explicitly mentioned
  auto sel = makeSelector({Encoding::deflate, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("*;q=0.9").encoding, Encoding::deflate);
  // Explicit better q wins
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.4, *;q=0.3").encoding, Encoding::gzip);
  // Wildcard lower q than explicit
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.9, *;q=0.1").encoding, Encoding::gzip);
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
    auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
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
    auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
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

TEST(AcceptEncodingNegotiationTest, TieBreakWithReversedPreferencesPicksDeflate) {
  if constexpr (aeronet::zlibEnabled()) {
    auto sel = makeSelector({Encoding::deflate, Encoding::gzip});
    EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.7, deflate;q=0.7").encoding, Encoding::deflate);
  } else {
    GTEST_SKIP() << "zlib disabled";
  }
}

TEST(AcceptEncodingNegotiationTest, HigherQOverridesPreferenceList) {
  if constexpr (aeronet::zlibEnabled()) {
    auto sel = makeSelector({Encoding::deflate});
    EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.9, deflate;q=0.8").encoding, Encoding::gzip);
  } else {
    GTEST_SKIP() << "zlib disabled";
  }
}

TEST(AcceptEncodingNegotiationTest, HigherQForPreferredBeatsUnlisted) {
  if constexpr (aeronet::zlibEnabled()) {
    auto sel = makeSelector({Encoding::gzip});
    EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.6, deflate;q=0.5").encoding, Encoding::gzip);
  } else {
    GTEST_SKIP() << "zlib disabled";
  }
}

TEST(AcceptEncodingNegotiationTest, WildcardSelectsFirstPreferenceWhenNoExplicit) {
  if constexpr (aeronet::zlibEnabled()) {
    auto sel = makeSelector({Encoding::deflate, Encoding::gzip});
    EXPECT_EQ(sel.negotiateAcceptEncoding("*;q=0.8").encoding, Encoding::deflate);
  } else {
    GTEST_SKIP() << "zlib disabled";
  }
}

TEST(AcceptEncodingNegotiationTest, WildcardDoesNotOverrideBetterExplicitQ) {
  if constexpr (aeronet::zlibEnabled()) {
    auto sel = makeSelector({Encoding::deflate, Encoding::gzip});
    EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.9, *;q=0.5").encoding, Encoding::gzip);
  } else {
    GTEST_SKIP() << "zlib disabled";
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

TEST(AcceptEncodingNegotiationTest, WildcardFillsForUnlistedWhenPositive) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP() << "zlib disabled";
  }
  CompressionConfig cfg;
  cfg.preferredFormats.push_back(Encoding::gzip);
  auto sel = EncodingSelector(cfg);
  EXPECT_EQ(sel.negotiateAcceptEncoding("*;q=0.6, gzip;q=0.6").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, ExplicitQZeroSkipsEncoding) {
#ifdef AERONET_ENABLE_ZLIB
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0, deflate;q=0.5").encoding, Encoding::deflate);
#else
  GTEST_SKIP() << "zlib disabled";
#endif
}

TEST(AcceptEncodingNegotiationTest, DuplicatesFirstOccurrenceWinsEvenIfLaterHigherQ) {
#ifdef AERONET_ENABLE_ZLIB
  auto sel = makeSelector({Encoding::deflate, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.2, gzip;q=0.9, deflate;q=0.2").encoding, Encoding::deflate);
#else
  GTEST_SKIP() << "zlib disabled";
#endif
}

TEST(AcceptEncodingNegotiationTest, DuplicateWithLowerQLaterDoesNotChangeChoice) {
#ifdef AERONET_ENABLE_ZLIB
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("deflate;q=0.8, deflate;q=0.1, gzip;q=0.9").encoding, Encoding::gzip);
#else
  GTEST_SKIP() << "zlib disabled";
#endif
}

TEST(AcceptEncodingNegotiationTest, InvalidQParsesAsZero) {
#ifdef AERONET_ENABLE_ZLIB
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=abc, deflate;q=0.4").encoding, Encoding::deflate);
#else
  GTEST_SKIP() << "zlib disabled";
#endif
}

TEST(AcceptEncodingNegotiationTest, QGreaterThanOneClamped) {
#ifdef AERONET_ENABLE_ZLIB
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("deflate;q=5, gzip;q=0.9").encoding, Encoding::deflate);
#else
  GTEST_SKIP() << "zlib disabled";
#endif
}

TEST(AcceptEncodingNegotiationTest, NegativeQClampedToZero) {
#ifdef AERONET_ENABLE_ZLIB
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=-1, deflate;q=0.3").encoding, Encoding::deflate);
#else
  GTEST_SKIP() << "zlib disabled";
#endif
}

TEST(AcceptEncodingNegotiationTest, IdentityExplicitHigherQChosenIfHigherQ) {
#ifdef AERONET_ENABLE_ZLIB
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
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
  cfg.preferredFormats.push_back(Encoding::gzip);
  auto sel = EncodingSelector(cfg);
  // Client prefers gzip slightly more, so gzip should be chosen despite server listing
  // identity first in its preference list.
  auto res = sel.negotiateAcceptEncoding("gzip;q=0.8, identity;q=0.7");
#ifdef AERONET_ENABLE_ZLIB
  EXPECT_EQ(res.encoding, Encoding::gzip);
#else
  EXPECT_EQ(res.encoding, Encoding::none);
#endif
}

TEST(AcceptEncodingNegotiationTest, IdentityPreferredButForbiddenSetsReject) {
  CompressionConfig cfg;
  cfg.preferredFormats.push_back(Encoding::none);
  cfg.preferredFormats.push_back(Encoding::gzip);
  auto sel = EncodingSelector(cfg);
  // If identity explicitly has q=0 and no alternatives acceptable, negotiation should set reject
  auto res = sel.negotiateAcceptEncoding("identity;q=0, gzip;q=0");
  EXPECT_EQ(res.encoding, Encoding::none);
  EXPECT_TRUE(res.reject);
}

TEST(AcceptEncodingNegotiationTest, IdentityPreferredAndChosenWhenHigherQ) {
  CompressionConfig cfg;
  cfg.preferredFormats.push_back(Encoding::none);
  cfg.preferredFormats.push_back(Encoding::gzip);
  auto sel = EncodingSelector(cfg);
  // If identity has a higher q than gzip, identity should be selected per client preference.
  auto res = sel.negotiateAcceptEncoding("identity;q=0.95, gzip;q=0.8");
  EXPECT_EQ(res.encoding, Encoding::none);
}

TEST(AcceptEncodingNegotiationTest, AllCompressionQZeroFallsBackToIdentity) {
#ifdef AERONET_ENABLE_ZLIB
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
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

TEST(AcceptEncodingNegotiationTest, MixedCaseAndSpacesRobust) {
#ifdef AERONET_ENABLE_ZLIB
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
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

TEST(AcceptEncodingNegotiationTest, TrailingCommasAndEmptyTokensIgnored) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
#ifdef AERONET_ENABLE_ZLIB
  EXPECT_EQ(sel.negotiateAcceptEncoding(",,,gzip;q=0.4,,deflate;q=0.4,,,").encoding, Encoding::gzip);
#else
  EXPECT_EQ(sel.negotiateAcceptEncoding(",,,gzip;q=0.4,,deflate;q=0.4,,,").encoding, Encoding::none);
#endif
}

TEST(AcceptEncodingNegotiationTest, IdentityExplicitlyForbiddenAndNoAlternativesSetsReject) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  auto resultReject = sel.negotiateAcceptEncoding("identity;q=0, gzip;q=0, deflate;q=0");
  EXPECT_EQ(resultReject.encoding, Encoding::none);
  EXPECT_TRUE(resultReject.reject);
}

TEST(AcceptEncodingNegotiationTest, ZstdPreferredWhenHighestQ) {
  if constexpr (!aeronet::zstdEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.preferredFormats.push_back(Encoding::gzip);
  cfg.preferredFormats.push_back(Encoding::deflate);
  cfg.preferredFormats.push_back(Encoding::zstd);
  auto sel = EncodingSelector(cfg);
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.7, zstd;q=0.9, deflate;q=0.8").encoding, Encoding::zstd);
}

TEST(AcceptEncodingNegotiationTest, ZstdViaWildcard) {
  if constexpr (!aeronet::zstdEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.preferredFormats.push_back(Encoding::zstd);
  cfg.preferredFormats.push_back(Encoding::gzip);
  auto sel = EncodingSelector(cfg);
  // zstd not explicitly listed -> wildcard applies; chosen because first preference
  EXPECT_EQ(sel.negotiateAcceptEncoding("*;q=0.5, gzip;q=0.5").encoding, Encoding::zstd);
}

TEST(AcceptEncodingNegotiationTest, WildcardMultiTierZstdDeflateTieBreak) {
  if constexpr (!aeronet::zstdEnabled()) {
    GTEST_SKIP();
  }
  // Server prefers gzip > zstd > deflate (enum order mirrored by explicit list missing gzip)
  CompressionConfig cfg;
  cfg.preferredFormats.push_back(Encoding::zstd);
  cfg.preferredFormats.push_back(Encoding::deflate);
  auto sel = EncodingSelector(cfg);
  // Client: wildcard gives q=0.8 to both unlisted (zstd, deflate); explicit deflate at same q as wildcard.
  // Result should pick zstd (higher server preference among tied q encodings that are candidates via wildcard or
  // explicit).
  EXPECT_EQ(sel.negotiateAcceptEncoding("deflate;q=0.8, *;q=0.8").encoding, Encoding::zstd);
  // Now raise deflate q so it wins if zlib is enabled
  if constexpr (aeronet::zlibEnabled()) {
    EXPECT_EQ(sel.negotiateAcceptEncoding("deflate;q=0.9, *;q=0.8").encoding, Encoding::deflate);
  }
}
#ifdef AERONET_ENABLE_BROTLI
TEST(AcceptEncodingNegotiationTest, BrotliPreferredWhenHighestQ) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate, Encoding::br});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.7, br;q=0.95, deflate;q=0.8").encoding, Encoding::br);
}

TEST(AcceptEncodingNegotiationTest, BrotliViaWildcard) {
  auto sel = makeSelector({Encoding::br, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("*;q=0.5, gzip;q=0.5").encoding, Encoding::br);
}
#endif

}  // namespace aeronet