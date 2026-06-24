#include "aeronet/jwt-algorithm.hpp"

#include <gtest/gtest.h>

#include "aeronet/jwt-algorithm-set.hpp"
#include "aeronet/jwt-error.hpp"

namespace aeronet {

namespace {
constexpr JwtAlgorithm kAll[] = {JwtAlgorithm::HS256, JwtAlgorithm::HS384, JwtAlgorithm::HS512, JwtAlgorithm::RS256,
                                 JwtAlgorithm::RS384, JwtAlgorithm::RS512, JwtAlgorithm::ES256, JwtAlgorithm::ES384,
                                 JwtAlgorithm::ES512, JwtAlgorithm::PS256, JwtAlgorithm::PS384, JwtAlgorithm::PS512,
                                 JwtAlgorithm::EdDSA};
}  // namespace

TEST(JwtAlgorithm, StringRoundTrip) {
  for (JwtAlgorithm alg : kAll) {
    JwtAlgorithm parsed{};
    ASSERT_TRUE(FromString(ToString(alg), parsed)) << ToString(alg);
    EXPECT_EQ(parsed, alg);
  }
}

TEST(JwtAlgorithm, KnownStrings) {
  EXPECT_EQ(ToString(JwtAlgorithm::HS256), "HS256");
  EXPECT_EQ(ToString(JwtAlgorithm::EdDSA), "EdDSA");
}

TEST(JwtAlgorithm, FromStringRejectsNoneAndUnknown) {
  JwtAlgorithm parsed{};
  EXPECT_FALSE(FromString("none", parsed));
  EXPECT_FALSE(FromString("None", parsed));
  EXPECT_FALSE(FromString("", parsed));
  EXPECT_FALSE(FromString("HS128", parsed));
  EXPECT_FALSE(FromString("RS999", parsed));
}

TEST(JwtAlgorithm, HmacClassification) {
  EXPECT_TRUE(IsHmac(JwtAlgorithm::HS256));
  EXPECT_TRUE(IsHmac(JwtAlgorithm::HS512));
  EXPECT_FALSE(IsHmac(JwtAlgorithm::RS256));
  EXPECT_FALSE(IsHmac(JwtAlgorithm::EdDSA));
}

TEST(JwtAlgorithmSet, EmptyByDefault) {
  JwtAlgorithmSet set;
  EXPECT_TRUE(set.empty());
  EXPECT_FALSE(set.contains(JwtAlgorithm::HS256));
}

TEST(JwtAlgorithmSet, AddAndContains) {
  JwtAlgorithmSet set;
  set.add(JwtAlgorithm::RS256).add(JwtAlgorithm::ES256);
  EXPECT_FALSE(set.empty());
  EXPECT_TRUE(set.contains(JwtAlgorithm::RS256));
  EXPECT_TRUE(set.contains(JwtAlgorithm::ES256));
  EXPECT_FALSE(set.contains(JwtAlgorithm::HS256));
}

TEST(JwtAlgorithmSet, InitializerList) {
  JwtAlgorithmSet set{JwtAlgorithm::HS256, JwtAlgorithm::EdDSA};
  EXPECT_TRUE(set.contains(JwtAlgorithm::HS256));
  EXPECT_TRUE(set.contains(JwtAlgorithm::EdDSA));
  EXPECT_FALSE(set.contains(JwtAlgorithm::HS384));
}

TEST(JwtError, AllMessagesNonEmpty) {
  EXPECT_EQ(ToString(JwtError::None), "ok");
  for (JwtError err :
       {JwtError::Malformed, JwtError::UnsupportedAlg, JwtError::AlgNotAllowed, JwtError::KeyMismatch,
        JwtError::InvalidSignature, JwtError::Expired, JwtError::NotYetValid, JwtError::MissingExpiration,
        JwtError::IssuerMismatch, JwtError::AudienceMismatch, JwtError::SubjectMismatch}) {
    EXPECT_FALSE(ToString(err).empty());
  }
}

}  // namespace aeronet
