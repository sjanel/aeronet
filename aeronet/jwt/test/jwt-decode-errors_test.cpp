#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/types.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/base64url.hpp"
#include "aeronet/jwt-algorithm-set.hpp"
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

// Sign `data` with HMAC-SHA256 and return the base64url signature (mirrors what the library emits).
std::string HmacSig(std::string_view secret, std::string_view data) {
  unsigned char mac[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  ::HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), mac, &len);
  return B64Url(std::string_view(reinterpret_cast<const char*>(mac), len));
}

JwtError DecodeErr(std::string_view token, const JwtKey& key, const JwtVerifyOptions& opts = {}) {
  JwtError err = JwtError::None;
  auto decoded = Jwt::tryDecode(token, key, opts, err);
  EXPECT_FALSE(decoded.valid());
  return err;
}

JwtVerifyOptions NoTemporal() {
  JwtVerifyOptions options;
  options.validateExpiration = false;
  options.validateNotBefore = false;
  return options;
}
}  // namespace

TEST(JwtDecodeErrors, StructuralShape) {
  JwtKey key = JwtKey::Hmac("secret");
  EXPECT_EQ(DecodeErr("no-dots-here", key), JwtError::Malformed);
  EXPECT_EQ(DecodeErr("only.one", key), JwtError::Malformed);
  EXPECT_EQ(DecodeErr("a.b.c.d", key), JwtError::Malformed);  // 4 parts (JWE-shaped)
}

TEST(JwtDecodeErrors, NoneAlgorithmRejected) {
  std::string token = B64Url(R"({"alg":"none","typ":"JWT"})") + "." + B64Url("{}") + ".";
  EXPECT_EQ(DecodeErr(token, JwtKey::Hmac("secret")), JwtError::UnsupportedAlg);
}

TEST(JwtDecodeErrors, UnknownOrMissingOrNonStringAlg) {
  JwtKey key = JwtKey::Hmac("secret");
  EXPECT_EQ(DecodeErr(B64Url(R"({"alg":"HS128"})") + "." + B64Url("{}") + ".x", key), JwtError::UnsupportedAlg);
  EXPECT_EQ(DecodeErr(B64Url(R"({"typ":"JWT"})") + "." + B64Url("{}") + ".x", key), JwtError::UnsupportedAlg);
  EXPECT_EQ(DecodeErr(B64Url(R"({"alg":123})") + "." + B64Url("{}") + ".x", key), JwtError::UnsupportedAlg);
}

TEST(JwtDecodeErrors, CritHeaderRejected) {
  std::string token = B64Url(R"({"alg":"HS256","crit":["exp"]})") + "." + B64Url("{}") + ".x";
  EXPECT_EQ(DecodeErr(token, JwtKey::Hmac("secret")), JwtError::Malformed);
}

TEST(JwtDecodeErrors, HeaderNotBase64OrNotObject) {
  JwtKey key = JwtKey::Hmac("secret");
  EXPECT_EQ(DecodeErr("@@@.{}.x", key), JwtError::Malformed);  // invalid base64url header
  EXPECT_EQ(DecodeErr(B64Url("123") + "." + B64Url("{}") + ".x", key), JwtError::Malformed);  // header is a number
}

TEST(JwtDecodeErrors, AlgorithmNotAllowed) {
  JwtKey key = JwtKey::Hmac("secret");
  std::string token = Jwt::encode("{}", key, JwtAlgorithm::HS256);
  JwtVerifyOptions opts = NoTemporal();
  opts.allowedAlgorithms = JwtAlgorithmSet{JwtAlgorithm::RS256, JwtAlgorithm::ES256};
  EXPECT_EQ(DecodeErr(token, key, opts), JwtError::AlgNotAllowed);
  // The same token passes when its algorithm is whitelisted.
  opts.allowedAlgorithms = JwtAlgorithmSet{JwtAlgorithm::HS256};
  JwtError err = JwtError::None;
  EXPECT_TRUE(Jwt::tryDecode(token, key, opts, err).valid());
}

TEST(JwtDecodeErrors, InvalidKeyIsMismatch) {
  std::string token = Jwt::encode("{}", JwtKey::Hmac("secret"), JwtAlgorithm::HS256);
  EXPECT_EQ(DecodeErr(token, JwtKey{}, NoTemporal()), JwtError::KeyMismatch);
}

TEST(JwtDecodeErrors, SignatureSegmentInvalidBase64) {
  JwtKey key = JwtKey::Hmac("secret");
  std::string token = B64Url(R"({"alg":"HS256"})") + "." + B64Url("{}") + ".@@@";
  EXPECT_EQ(DecodeErr(token, key, NoTemporal()), JwtError::Malformed);
}

TEST(JwtDecodeErrors, PayloadInvalidBase64ButSignedOver) {
  // A validly-signed token whose payload segment is not base64url: signature passes, then the
  // payload fails to decode.
  std::string headerB64 = B64Url(R"({"alg":"HS256","typ":"JWT"})");
  std::string payloadSeg = "@@not-base64@@";
  std::string signingInput = headerB64 + "." + payloadSeg;
  std::string token = signingInput + "." + HmacSig("secret", signingInput);
  EXPECT_EQ(DecodeErr(token, JwtKey::Hmac("secret"), NoTemporal()), JwtError::Malformed);
}

TEST(JwtDecodeErrors, PayloadNotJsonOrNotObject) {
  JwtKey key = JwtKey::Hmac("secret");
  // encode() does not validate the payload, so we can sign non-object payloads to exercise decode.
  EXPECT_EQ(DecodeErr(Jwt::encode("not json", key, JwtAlgorithm::HS256), key, NoTemporal()), JwtError::Malformed);
  EXPECT_EQ(DecodeErr(Jwt::encode("123", key, JwtAlgorithm::HS256), key, NoTemporal()), JwtError::Malformed);
}

TEST(JwtDecodeErrors, EcdsaWrongSignatureLength) {
  test::TestKey ec = test::GenerateEc("P-256");
  std::string headerB64 = B64Url(R"({"alg":"ES256"})");
  std::string token = headerB64 + "." + B64Url("{}") + "." + B64Url("too-short");  // 9-byte "signature"
  EXPECT_EQ(DecodeErr(token, JwtKey::FromPem(ec.publicPem), NoTemporal()), JwtError::InvalidSignature);
}

TEST(JwtKeyErrors, FactoriesReturnInvalidOnBadInput) {
  EXPECT_FALSE(JwtKey::Hmac("").valid());  // empty secret
  EXPECT_FALSE(JwtKey::FromPem("not a pem").valid());
  EXPECT_FALSE(static_cast<bool>(JwtKey::FromPem("not a pem")));
}

TEST(JwtKeyErrors, UnsupportedKeyTypeIsInvalid) {
  // A valid PEM, but of a key type JWS does not use (X25519 is for key agreement, not signing).
  EVP_PKEY* key = ::EVP_PKEY_Q_keygen(nullptr, nullptr, "X25519");
  ASSERT_NE(key, nullptr);
  BIO* bio = ::BIO_new(BIO_s_mem());
  ::PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
  char* data = nullptr;
  const long len = ::BIO_get_mem_data(bio, &data);
  std::string pem(data, static_cast<std::size_t>(len));
  ::BIO_free(bio);
  ::EVP_PKEY_free(key);
  EXPECT_FALSE(JwtKey::FromPem(pem).valid());
}

}  // namespace aeronet
