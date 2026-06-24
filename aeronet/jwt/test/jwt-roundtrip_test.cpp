#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

#include "aeronet/jwt-algorithm.hpp"
#include "aeronet/jwt-error.hpp"
#include "aeronet/jwt-key.hpp"
#include "aeronet/jwt.hpp"
#include "jwt-test-keys.hpp"

namespace aeronet {

namespace {
constexpr std::string_view kClaims = R"({"sub":"1234567890","name":"Jane Doe","admin":true})";

// Decode helper that disables temporal checks (the fixed claims carry no exp/nbf).
JwtVerifyOptions NoTemporal() {
  JwtVerifyOptions opts;
  opts.validateExpiration = false;
  opts.validateNotBefore = false;
  return opts;
}

DecodedJwt Decode(std::string_view token, const JwtKey& key) {
  JwtError err = JwtError::None;
  return Jwt::tryDecode(token, key, NoTemporal(), err);
}
}  // namespace

TEST(JwtRoundTrip, HmacAllSizes) {
  JwtKey key = JwtKey::Hmac("a-very-secret-shared-key");
  for (JwtAlgorithm alg : {JwtAlgorithm::HS256, JwtAlgorithm::HS384, JwtAlgorithm::HS512}) {
    std::string token = Jwt::encode(kClaims, key, alg);
    JwtError err = JwtError::None;
    auto decoded = Jwt::tryDecode(token, key, NoTemporal(), err);
    ASSERT_TRUE(decoded.valid()) << ToString(err) << " alg=" << ToString(alg);
    EXPECT_EQ(decoded.algorithm(), alg);
    EXPECT_EQ(decoded.subject(), "1234567890");
    EXPECT_EQ(decoded.payloadJson(), kClaims);
  }
}

TEST(JwtRoundTrip, HmacWrongSecretFails) {
  std::string token = Jwt::encode(kClaims, JwtKey::Hmac("right"), JwtAlgorithm::HS256);
  JwtError err = JwtError::None;
  auto decoded = Jwt::tryDecode(token, JwtKey::Hmac("wrong"), NoTemporal(), err);
  EXPECT_FALSE(decoded.valid());
  EXPECT_EQ(err, JwtError::InvalidSignature);
}

TEST(JwtRoundTrip, TamperedPayloadFails) {
  JwtKey key = JwtKey::Hmac("secret");
  std::string token = Jwt::encode(kClaims, key, JwtAlgorithm::HS256);
  token[token.size() - 3] = (token[token.size() - 3] == 'A') ? 'B' : 'A';  // flip a signature char
  JwtError err = JwtError::None;
  EXPECT_FALSE(Jwt::tryDecode(token, key, NoTemporal(), err).valid());
  EXPECT_EQ(err, JwtError::InvalidSignature);
}

TEST(JwtRoundTrip, KeyId) {
  JwtKey key = JwtKey::Hmac("secret");
  std::string token = Jwt::encode(kClaims, key, JwtAlgorithm::HS256, "key-2024");
  auto decoded = Decode(token, key);
  ASSERT_TRUE(decoded.valid());
  EXPECT_EQ(decoded.keyId(), "key-2024");
  EXPECT_EQ(decoded.type(), "JWT");
}

TEST(JwtRoundTrip, Rsa) {
  test::TestKey rsa = test::GenerateRsa();
  JwtKey priv = JwtKey::FromPem(rsa.privatePem);
  JwtKey pub = JwtKey::FromPem(rsa.publicPem);
  for (JwtAlgorithm alg : {JwtAlgorithm::RS256, JwtAlgorithm::RS384, JwtAlgorithm::RS512, JwtAlgorithm::PS256,
                           JwtAlgorithm::PS384, JwtAlgorithm::PS512}) {
    std::string token = Jwt::encode(kClaims, priv, alg);
    JwtError err = JwtError::None;
    auto decoded = Jwt::tryDecode(token, pub, NoTemporal(), err);
    ASSERT_TRUE(decoded.valid()) << ToString(err) << " alg=" << ToString(alg);
    EXPECT_EQ(decoded.algorithm(), alg);
  }
}

TEST(JwtRoundTrip, Ecdsa) {
  struct Case {
    const char* curve;
    JwtAlgorithm alg;
  };
  for (Case tc :
       {Case{"P-256", JwtAlgorithm::ES256}, Case{"P-384", JwtAlgorithm::ES384}, Case{"P-521", JwtAlgorithm::ES512}}) {
    test::TestKey ec = test::GenerateEc(tc.curve);
    std::string token = Jwt::encode(kClaims, JwtKey::FromPem(ec.privatePem), tc.alg);
    JwtError err = JwtError::None;
    auto decoded = Jwt::tryDecode(token, JwtKey::FromPem(ec.publicPem), NoTemporal(), err);
    ASSERT_TRUE(decoded.valid()) << ToString(err) << " curve=" << tc.curve;
    EXPECT_EQ(decoded.algorithm(), tc.alg);
    // A second signature over the same input differs (ECDSA is randomized) yet still verifies.
    std::string token2 = Jwt::encode(kClaims, JwtKey::FromPem(ec.privatePem), tc.alg);
    EXPECT_TRUE(Jwt::tryDecode(token2, JwtKey::FromPem(ec.publicPem), NoTemporal(), err).valid());
  }
}

TEST(JwtRoundTrip, EdDsa) {
  test::TestKey ed = test::GenerateEd25519();
  std::string token = Jwt::encode(kClaims, JwtKey::FromPem(ed.privatePem), JwtAlgorithm::EdDSA);
  JwtError err = JwtError::None;
  auto decoded = Jwt::tryDecode(token, JwtKey::FromPem(ed.publicPem), NoTemporal(), err);
  ASSERT_TRUE(decoded.valid()) << ToString(err);
  EXPECT_EQ(decoded.algorithm(), JwtAlgorithm::EdDSA);
}

TEST(JwtRoundTrip, AlgorithmKeyFamilyMismatch) {
  test::TestKey rsa = test::GenerateRsa();
  JwtKey rsaPriv = JwtKey::FromPem(rsa.privatePem);
  // Asking an RSA key to sign with an HMAC algorithm is refused: encode returns an empty token.
  EXPECT_TRUE(Jwt::encode(kClaims, rsaPriv, JwtAlgorithm::HS256).empty());

  // A token signed RS256 cannot be verified with an HMAC key: family mismatch.
  std::string token = Jwt::encode(kClaims, rsaPriv, JwtAlgorithm::RS256);
  JwtError err = JwtError::None;
  EXPECT_FALSE(Jwt::tryDecode(token, JwtKey::Hmac("secret"), NoTemporal(), err).valid());
  EXPECT_EQ(err, JwtError::KeyMismatch);
}

TEST(JwtKeyLifecycle, BoolConversionAndMove) {
  JwtKey key = JwtKey::Hmac("s");
  EXPECT_TRUE(static_cast<bool>(key));
  JwtKey empty;
  EXPECT_FALSE(static_cast<bool>(empty));
  empty = std::move(key);  // move-assignment into an empty key
  EXPECT_TRUE(empty.valid());
  JwtKey moved(std::move(empty));  // move-construction
  EXPECT_TRUE(moved.valid());
}

TEST(JwtRoundTrip, HeaderAndClaimAccessors) {
  JwtKey key = JwtKey::Hmac("secret");
  // A kid exercising every JSON escape branch: quote, backslash, \n, \r, \t and a control byte.
  const std::string special = "ki\"d\\\n\r\t\x01";
  std::string token = Jwt::encode(R"({"jti":"id-123"})", key, JwtAlgorithm::HS256, special);
  auto decoded = Decode(token, key);
  ASSERT_TRUE(decoded.valid());
  EXPECT_EQ(decoded.keyId(), special);  // round-trips through JSON escaping
  EXPECT_EQ(decoded.jwtId(), "id-123");
  EXPECT_TRUE(decoded.headerJson().contains("HS256"));
  // Absent claims surface as the natural empty state (empty view / 0 NumericDate).
  EXPECT_TRUE(decoded.subject().empty());
  EXPECT_TRUE(decoded.issuer().empty());
  EXPECT_EQ(decoded.expiresAt(), 0);
}

TEST(JwtRoundTrip, LargePayload) {
  JwtKey key = JwtKey::Hmac("secret");
  std::string big = R"({"data":")";
  big.append(20000, 'x');
  big += R"("})";
  std::string token = Jwt::encode(big, key, JwtAlgorithm::HS256);
  JwtError err = JwtError::None;
  auto decoded = Jwt::tryDecode(token, key, NoTemporal(), err);
  ASSERT_TRUE(decoded.valid()) << ToString(err);
  EXPECT_EQ(decoded.payloadJson(), big);
}

}  // namespace aeronet
