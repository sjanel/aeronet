#include <gtest/gtest.h>

#include <span>
#include <string>
#include <string_view>

#include "aeronet/base64url.hpp"
#include "aeronet/jwks.hpp"
#include "aeronet/jwt-algorithm.hpp"
#include "aeronet/jwt-error.hpp"
#include "aeronet/jwt-key.hpp"
#include "aeronet/jwt.hpp"
#include "jwt-test-keys.hpp"

namespace aeronet {

namespace {
std::string B64Url(std::string_view in) {
  std::string out(B64UrlEncodedLen(in.size()), '\0');
  B64UrlEncode(std::span<const char>(in.data(), in.size()), out.data());
  return out;
}

std::string OctJwk(std::string_view kid, std::string_view secret) {
  return std::string(R"({"kty":"oct","kid":")") + std::string(kid) + R"(","k":")" + B64Url(secret) + R"("})";
}

JwtVerifyOptions NoTemporal() {
  JwtVerifyOptions options;
  options.validateExpiration = false;
  options.validateNotBefore = false;
  return options;
}
}  // namespace

TEST(Jwk, OctRoundTrip) {
  const std::string_view secret = "shared-hmac-secret-material";
  std::string token = Jwt::encode(R"({"sub":"x"})", JwtKey::Hmac(secret), JwtAlgorithm::HS256);
  JwtKey key = JwtKey::FromJwk(OctJwk("k1", secret));
  EXPECT_EQ(key.keyId(), "k1");
  JwtError err = JwtError::None;
  EXPECT_TRUE(Jwt::tryDecode(token, key, NoTemporal(), err).valid()) << ToString(err);
}

TEST(Jwk, RsaPublicFromJwk) {
  test::TestKey rsa = test::GenerateRsa();
  std::string token = Jwt::encode(R"({"sub":"x"})", JwtKey::FromPem(rsa.privatePem), JwtAlgorithm::RS256);
  JwtError err = JwtError::None;
  EXPECT_TRUE(Jwt::tryDecode(token, JwtKey::FromJwk(rsa.jwk), NoTemporal(), err).valid()) << ToString(err);
}

TEST(Jwk, EcPublicFromJwk) {
  for (const char* curve : {"P-256", "P-384", "P-521"}) {
    test::TestKey ec = test::GenerateEc(curve);
    JwtAlgorithm alg;
    if (std::string_view(curve) == "P-256") {
      alg = JwtAlgorithm::ES256;
    } else if (std::string_view(curve) == "P-384") {
      alg = JwtAlgorithm::ES384;
    } else {
      alg = JwtAlgorithm::ES512;
    }
    std::string token = Jwt::encode(R"({"sub":"x"})", JwtKey::FromPem(ec.privatePem), alg);
    JwtError err = JwtError::None;
    EXPECT_TRUE(Jwt::tryDecode(token, JwtKey::FromJwk(ec.jwk), NoTemporal(), err).valid())
        << ToString(err) << " " << curve;
  }
}

TEST(Jwk, OkpEd25519FromJwk) {
  test::TestKey ed = test::GenerateEd25519();
  std::string token = Jwt::encode(R"({"sub":"x"})", JwtKey::FromPem(ed.privatePem), JwtAlgorithm::EdDSA);
  JwtError err = JwtError::None;
  EXPECT_TRUE(Jwt::tryDecode(token, JwtKey::FromJwk(ed.jwk), NoTemporal(), err).valid()) << ToString(err);
}

TEST(Jwk, FactoryErrorsReturnInvalid) {
  EXPECT_FALSE(JwtKey::FromJwk("not json").valid());
  EXPECT_FALSE(JwtKey::FromJwk(R"({"kty":"FOO"})").valid());
  EXPECT_FALSE(JwtKey::FromJwk(R"({"kty":"oct","k":""})").valid());
  EXPECT_FALSE(JwtKey::FromJwk(R"({"kty":"EC","crv":"P-999","x":"AA","y":"AA"})").valid());
  // Valid curve but a coordinate of the wrong byte length.
  EXPECT_FALSE(JwtKey::FromJwk(R"({"kty":"EC","crv":"P-256","x":"AA","y":"AA"})").valid());
  EXPECT_FALSE(JwtKey::FromJwk(R"({"kty":"OKP","crv":"X25519","x":"AA"})").valid());
  EXPECT_FALSE(JwtKey::FromJwk(R"({"kty":"oct","k":"@@@"})").valid());
  // Invalid base64url inside the RSA / EC coordinate fields.
  EXPECT_FALSE(JwtKey::FromJwk(R"({"kty":"RSA","n":"@@@","e":"AQAB"})").valid());
  EXPECT_FALSE(JwtKey::FromJwk(R"({"kty":"EC","crv":"P-256","x":"@@@","y":"AA"})").valid());
  // Correct coordinate lengths, but (0,0) is not a point on P-256 → key build fails.
  const std::string zero32(43, 'A');  // 43 base64url 'A's decode to 32 zero bytes
  EXPECT_FALSE(
      JwtKey::FromJwk(std::string(R"({"kty":"EC","crv":"P-256","x":")") + zero32 + R"(","y":")" + zero32 + R"("})")
          .valid());
  // OKP with a raw public key of the wrong length for Ed25519.
  EXPECT_FALSE(JwtKey::FromJwk(R"({"kty":"OKP","crv":"Ed25519","x":"AA"})").valid());
}

TEST(Jwks, ParseFindAndSize) {
  std::string doc = std::string(R"({"keys":[)") + OctJwk("k1", "secret-one") + "," + OctJwk("k2", "secret-two") +
                    R"(,{"kty":"UNSUPPORTED"}]})";
  Jwks set(doc);
  EXPECT_EQ(set.size(), 2U);  // the unsupported key was skipped
  EXPECT_FALSE(set.empty());
  EXPECT_NE(set.find("k1"), nullptr);
  EXPECT_NE(set.find("k2"), nullptr);
  EXPECT_EQ(set.find("missing"), nullptr);
}

TEST(Jwks, DecodeSelectsByKid) {
  std::string doc = std::string(R"({"keys":[)") + OctJwk("k1", "secret-one") + "," + OctJwk("k2", "secret-two") + "]}";
  Jwks set(doc);
  std::string token = Jwt::encode(R"({"sub":"x"})", JwtKey::Hmac("secret-two"), JwtAlgorithm::HS256, "k2");
  JwtError err = JwtError::None;
  auto decoded = set.tryDecode(token, NoTemporal(), err);
  ASSERT_TRUE(decoded.valid()) << ToString(err);
  EXPECT_EQ(decoded.subject(), "x");
}

TEST(Jwks, UnknownKidIsMismatch) {
  Jwks set(std::string(R"({"keys":[)") + OctJwk("k1", "s1") + "]}");
  std::string token = Jwt::encode("{}", JwtKey::Hmac("s1"), JwtAlgorithm::HS256, "other-kid");
  JwtError err = JwtError::None;
  EXPECT_FALSE(set.tryDecode(token, NoTemporal(), err).valid());
  EXPECT_EQ(err, JwtError::KeyMismatch);
}

TEST(Jwks, NoKidSingleKeyUsed) {
  Jwks set(std::string(R"({"keys":[)") + OctJwk("k1", "s1") + "]}");
  std::string token = Jwt::encode("{}", JwtKey::Hmac("s1"), JwtAlgorithm::HS256);  // no kid in header
  JwtError err = JwtError::None;
  EXPECT_TRUE(set.tryDecode(token, NoTemporal(), err).valid()) << ToString(err);
}

TEST(Jwks, NoKidMultipleKeysAmbiguous) {
  Jwks set(std::string(R"({"keys":[)") + OctJwk("k1", "s1") + "," + OctJwk("k2", "s2") + "]}");
  std::string token = Jwt::encode("{}", JwtKey::Hmac("s1"), JwtAlgorithm::HS256);  // no kid
  JwtError err = JwtError::None;
  EXPECT_FALSE(set.tryDecode(token, NoTemporal(), err).valid());
  EXPECT_EQ(err, JwtError::KeyMismatch);
}

TEST(Jwks, MalformedDocumentYieldsEmptySet) {
  EXPECT_TRUE(Jwks("not json").empty());
  EXPECT_TRUE(Jwks(R"({"nokeys":1})").empty());
  EXPECT_TRUE(Jwks(R"({"keys":"notarray"})").empty());
}

TEST(Jwks, DecodeMalformedTokenShape) {
  Jwks set(std::string(R"({"keys":[)") + OctJwk("k1", "s1") + "]}");
  JwtError err = JwtError::None;
  EXPECT_FALSE(set.tryDecode("nodots", NoTemporal(), err).valid());
  EXPECT_EQ(err, JwtError::Malformed);
  EXPECT_FALSE(set.tryDecode("@@@.payload.sig", NoTemporal(), err).valid());
  EXPECT_EQ(err, JwtError::Malformed);
  // Header decodes as valid base64url + JSON, but is not an object.
  EXPECT_FALSE(set.tryDecode(B64Url("42") + ".e30.x", NoTemporal(), err).valid());
  EXPECT_EQ(err, JwtError::Malformed);
}

}  // namespace aeronet
