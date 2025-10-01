#include "accept-encoding-negotiation.hpp"

#include <gtest/gtest.h>

#include <initializer_list>  // std::initializer_list
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"

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
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip").encoding, Encoding::gzip);
  EXPECT_EQ(sel.negotiateAcceptEncoding("deflate").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, CaseInsensitive) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("GZIP").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, WithParametersOrderAndQ) {
  // Prefer higher q even if later
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.5, deflate;q=0.9").encoding, Encoding::deflate);
  // Tie in q -> server preference order (gzip preferred over deflate when equal q with current prefs)
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.8, deflate;q=0.8").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, IdentityFallback) {
  // All unsupported or q=0 -> identity
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("brotli, lz4").encoding, Encoding::none);
  {
    auto resultAllZero = sel.negotiateAcceptEncoding("gzip;q=0, deflate;q=0");
    EXPECT_EQ(resultAllZero.encoding, Encoding::none);
    EXPECT_FALSE(resultAllZero.reject) << "Identity not explicitly forbidden so reject flag must be false";
  }
}

TEST(AcceptEncodingNegotiationTest, Wildcard) {
  // Wildcard picks first server preference not explicitly mentioned
  auto sel = makeSelector({Encoding::deflate, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("*;q=0.9").encoding, Encoding::deflate);
  // Explicit better q wins
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.4, *;q=0.3").encoding, Encoding::gzip);
  // Wildcard lower q than explicit
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.9, *;q=0.1").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, IgnoreUnsupportedWithWildcard) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("br;q=0.9, *;q=0.5").encoding,
            Encoding::gzip);  // brotli unsupported -> wildcard
}

TEST(AcceptEncodingNegotiationTest, InvalidQValues) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=abc, deflate").encoding,
            Encoding::deflate);  // invalid q for gzip treated as 0
}

TEST(AcceptEncodingNegotiationTest, SpacesAndTabs) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding(" gzip ; q=1 , deflate ; q=0.4").encoding, Encoding::gzip);
}

// Additional comprehensive scenarios covering tie-breaks, wildcard, duplicates, and preference nuances.

TEST(AcceptEncodingNegotiationTest, TieBreakNoPreferencesUsesEnumOrder) {
  CompressionConfig cfg;  // no preferredFormats -> enum order gzip, deflate, none
  EncodingSelector sel(cfg);
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.7, deflate;q=0.7").encoding, Encoding::gzip);  // enum order tie
}

TEST(AcceptEncodingNegotiationTest, TieBreakWithReversedPreferencesPicksDeflate) {
  auto sel = makeSelector({Encoding::deflate, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.7, deflate;q=0.7").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, HigherQOverridesPreferenceList) {
  // Preferred list favors deflate but gzip has higher q
  auto sel = makeSelector({Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.9, deflate;q=0.8").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, HigherQForPreferredBeatsUnlisted) {
  // Preferred list favors gzip; gzip has higher q anyway
  auto sel = makeSelector({Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.6, deflate;q=0.5").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, WildcardSelectsFirstPreferenceWhenNoExplicit) {
  auto sel = makeSelector({Encoding::deflate, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("*;q=0.8").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, WildcardDoesNotOverrideBetterExplicitQ) {
  auto sel = makeSelector({Encoding::deflate, Encoding::gzip});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.9, *;q=0.5").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, WildcardZeroPreventsUnlistedSelection) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  // Explicit gzip q=0 -> not acceptable; wildcard q=0 -> no others acceptable -> identity (none)
  {
    auto resultWildcardAllZero = sel.negotiateAcceptEncoding("gzip;q=0, *;q=0");
    EXPECT_EQ(resultWildcardAllZero.encoding, Encoding::none);
    EXPECT_FALSE(resultWildcardAllZero.reject);
  }
}

TEST(AcceptEncodingNegotiationTest, WildcardFillsForUnlistedWhenPositive) {
  auto sel = makeSelector({Encoding::gzip});
  // deflate not in preference list but wildcard gives it same q; only gzip in list so gzip wins tie
  EXPECT_EQ(sel.negotiateAcceptEncoding("*;q=0.6, gzip;q=0.6").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, ExplicitQZeroSkipsEncoding) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0, deflate;q=0.5").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, DuplicatesFirstOccurrenceWinsEvenIfLaterHigherQ) {
  auto sel = makeSelector({Encoding::deflate, Encoding::gzip});
  // First gzip q=0.2 captured; later gzip;q=0.9 ignored; deflate q=0.2 tie broken by preference (deflate first)
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=0.2, gzip;q=0.9, deflate;q=0.2").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, DuplicateWithLowerQLaterDoesNotChangeChoice) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  // First deflate q=0.8 captured; later deflate;q=0.1 ignored; higher gzip q=0.9 wins anyway
  EXPECT_EQ(sel.negotiateAcceptEncoding("deflate;q=0.8, deflate;q=0.1, gzip;q=0.9").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, InvalidQParsesAsZero) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=abc, deflate;q=0.4").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, QGreaterThanOneClamped) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  // q=5 treated as 1.0
  EXPECT_EQ(sel.negotiateAcceptEncoding("deflate;q=5, gzip;q=0.9").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, NegativeQClampedToZero) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("gzip;q=-1, deflate;q=0.3").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, IdentityExplicitHigherQChosenIfHigherQ) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  // Current implementation: identity participates; if it has strictly higher q it wins.
  {
    auto resultIdentityHigh = sel.negotiateAcceptEncoding("identity;q=1, gzip;q=0.8");
    EXPECT_EQ(resultIdentityHigh.encoding, Encoding::none);
    EXPECT_FALSE(resultIdentityHigh.reject);
  }
}

TEST(AcceptEncodingNegotiationTest, AllCompressionQZeroFallsBackToIdentity) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  {
    auto resultAllCompressionZero = sel.negotiateAcceptEncoding("gzip;q=0, deflate;q=0");
    EXPECT_EQ(resultAllCompressionZero.encoding, Encoding::none);
    EXPECT_FALSE(resultAllCompressionZero.reject);
  }
}

TEST(AcceptEncodingNegotiationTest, MixedCaseAndSpacesRobust) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding("  GzIp ; Q=0.7 ,  DeFlAtE ; q=0.9  ").encoding, Encoding::deflate);
}

TEST(AcceptEncodingNegotiationTest, TrailingCommasAndEmptyTokensIgnored) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  EXPECT_EQ(sel.negotiateAcceptEncoding(",,,gzip;q=0.4,,deflate;q=0.4,,,").encoding, Encoding::gzip);
}

TEST(AcceptEncodingNegotiationTest, IdentityExplicitlyForbiddenAndNoAlternativesSetsReject) {
  auto sel = makeSelector({Encoding::gzip, Encoding::deflate});
  auto resultReject = sel.negotiateAcceptEncoding("identity;q=0, gzip;q=0, deflate;q=0");
  EXPECT_EQ(resultReject.encoding, Encoding::none);
  EXPECT_TRUE(resultReject.reject);
}

}  // namespace aeronet