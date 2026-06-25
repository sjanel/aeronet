#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>

#include "aeronet/jwt-error.hpp"
#include "aeronet/jwt-key.hpp"
#include "aeronet/jwt.hpp"

namespace aeronet {

namespace {
const JwtKey& Key() {
  static JwtKey key = JwtKey::Hmac("test-secret");
  return key;
}

std::string Sign(std::string_view claimsJson) { return Jwt::encode(claimsJson, Key(), JwtAlgorithm::HS256); }

// A reference clock at t=2000s so temporal cases are deterministic.
std::chrono::system_clock::time_point FixedNow() {
  return std::chrono::system_clock::time_point(std::chrono::seconds(2000));
}
}  // namespace

TEST(JwtClaims, ExpiredRejected) {
  std::string token = Sign(R"({"exp":1000})");
  JwtVerifyOptions opts;
  opts.clock = FixedNow();
  JwtError err = JwtError::None;
  EXPECT_FALSE(Jwt::tryDecode(token, Key(), opts, err).valid());
  EXPECT_EQ(err, JwtError::Expired);
}

TEST(JwtClaims, ExpiredWithinLeewayAccepted) {
  std::string token = Sign(R"({"exp":1000})");
  JwtVerifyOptions opts;
  opts.clock = FixedNow();
  opts.leeway = std::chrono::seconds(2000);  // 2000 - 1000 = 1000 <= leeway
  JwtError err = JwtError::None;
  EXPECT_TRUE(Jwt::tryDecode(token, Key(), opts, err).valid()) << ToString(err);
}

TEST(JwtClaims, NotYetValidRejected) {
  std::string token = Sign(R"({"nbf":5000})");
  JwtVerifyOptions opts;
  opts.clock = FixedNow();
  JwtError err = JwtError::None;
  EXPECT_FALSE(Jwt::tryDecode(token, Key(), opts, err).valid());
  EXPECT_EQ(err, JwtError::NotYetValid);
}

TEST(JwtClaims, NotBeforeWithinLeewayAccepted) {
  std::string token = Sign(R"({"nbf":5000})");
  JwtVerifyOptions opts;
  opts.clock = FixedNow();
  opts.leeway = std::chrono::seconds(4000);
  JwtError err = JwtError::None;
  EXPECT_TRUE(Jwt::tryDecode(token, Key(), opts, err).valid()) << ToString(err);
}

TEST(JwtClaims, RequireExpirationMissing) {
  std::string token = Sign(R"({"sub":"x"})");
  JwtVerifyOptions opts;
  opts.requireExpiration = true;
  JwtError err = JwtError::None;
  EXPECT_FALSE(Jwt::tryDecode(token, Key(), opts, err).valid());
  EXPECT_EQ(err, JwtError::MissingExpiration);
}

TEST(JwtClaims, ExpirationCheckDisabled) {
  std::string token = Sign(R"({"exp":1000})");
  JwtVerifyOptions opts;
  opts.clock = FixedNow();
  opts.validateExpiration = false;
  JwtError err = JwtError::None;
  EXPECT_TRUE(Jwt::tryDecode(token, Key(), opts, err).valid()) << ToString(err);
}

TEST(JwtClaims, IssuerMatch) {
  std::string token = Sign(R"({"iss":"https://issuer.example","exp":9999})");
  JwtVerifyOptions opts;
  opts.clock = FixedNow();
  opts.issuer = "https://issuer.example";
  JwtError err = JwtError::None;
  auto decoded = Jwt::tryDecode(token, Key(), opts, err);
  ASSERT_TRUE(decoded.valid()) << ToString(err);
  EXPECT_EQ(decoded.issuer(), "https://issuer.example");
}

TEST(JwtClaims, IssuerMismatchAndAbsent) {
  JwtVerifyOptions opts;
  opts.clock = FixedNow();
  opts.issuer = "expected";
  JwtError err = JwtError::None;
  EXPECT_FALSE(Jwt::tryDecode(Sign(R"({"iss":"other"})"), Key(), opts, err).valid());
  EXPECT_EQ(err, JwtError::IssuerMismatch);
  EXPECT_FALSE(Jwt::tryDecode(Sign(R"({"sub":"x"})"), Key(), opts, err).valid());
  EXPECT_EQ(err, JwtError::IssuerMismatch);
}

TEST(JwtClaims, SubjectMismatch) {
  JwtVerifyOptions opts;
  opts.clock = FixedNow();
  opts.subject = "user-1";
  JwtError err = JwtError::None;
  EXPECT_FALSE(Jwt::tryDecode(Sign(R"({"sub":"user-2"})"), Key(), opts, err).valid());
  EXPECT_EQ(err, JwtError::SubjectMismatch);
  EXPECT_TRUE(Jwt::tryDecode(Sign(R"({"sub":"user-1"})"), Key(), opts, err).valid());
}

TEST(JwtClaims, AudienceAsString) {
  JwtVerifyOptions opts;
  opts.clock = FixedNow();
  opts.audience = "api://default";
  JwtError err = JwtError::None;
  EXPECT_TRUE(Jwt::tryDecode(Sign(R"({"aud":"api://default"})"), Key(), opts, err).valid());
  EXPECT_FALSE(Jwt::tryDecode(Sign(R"({"aud":"other"})"), Key(), opts, err).valid());
  EXPECT_EQ(err, JwtError::AudienceMismatch);
}

TEST(JwtClaims, AudienceAsArray) {
  std::string token = Sign(R"({"aud":["one","api://default","three"]})");
  JwtVerifyOptions opts;
  opts.clock = FixedNow();
  opts.audience = "api://default";
  JwtError err = JwtError::None;
  auto decoded = Jwt::tryDecode(token, Key(), opts, err);
  ASSERT_TRUE(decoded.valid()) << ToString(err);
  EXPECT_EQ(decoded.audiences().size(), 3U);
  EXPECT_TRUE(decoded.hasAudience("one"));
  EXPECT_TRUE(decoded.hasAudience("three"));
  EXPECT_FALSE(decoded.hasAudience("missing"));
}

TEST(JwtClaims, TimeAccessors) {
  std::string token = Sign(R"({"exp":9999,"nbf":1500,"iat":1000})");
  JwtVerifyOptions opts;
  opts.clock = FixedNow();
  JwtError err = JwtError::None;
  auto decoded = Jwt::tryDecode(token, Key(), opts, err);
  ASSERT_TRUE(decoded.valid()) << ToString(err);
  EXPECT_EQ(decoded.expiresAt(), 9999);  // NumericDate (seconds since epoch), 0 == absent
  EXPECT_EQ(decoded.notBefore(), 1500);
  EXPECT_EQ(decoded.issuedAt(), 1000);
}

TEST(JwtClaims, CustomClaimsAvailableViaPayloadJson) {
  std::string_view claims = R"({"role":"admin","scopes":["read","write"]})";
  std::string token = Sign(claims);
  JwtVerifyOptions opts;
  opts.validateExpiration = false;
  JwtError err = JwtError::None;
  auto decoded = Jwt::tryDecode(token, Key(), opts, err);
  ASSERT_TRUE(decoded.valid()) << ToString(err);
  EXPECT_EQ(decoded.payloadJson(), claims);
}

TEST(JwtClaims, MalformedRegisteredClaimTypes) {
  JwtVerifyOptions opts;
  opts.validateExpiration = false;
  JwtError err = JwtError::None;
  EXPECT_FALSE(Jwt::tryDecode(Sign(R"({"exp":"soon"})"), Key(), opts, err).valid());
  EXPECT_EQ(err, JwtError::Malformed);
  EXPECT_FALSE(Jwt::tryDecode(Sign(R"({"iss":123})"), Key(), opts, err).valid());
  EXPECT_EQ(err, JwtError::Malformed);
  EXPECT_FALSE(Jwt::tryDecode(Sign(R"({"aud":123})"), Key(), opts, err).valid());
  EXPECT_EQ(err, JwtError::Malformed);
  EXPECT_FALSE(Jwt::tryDecode(Sign(R"({"aud":[1,2]})"), Key(), opts, err).valid());
  EXPECT_EQ(err, JwtError::Malformed);
}

}  // namespace aeronet
