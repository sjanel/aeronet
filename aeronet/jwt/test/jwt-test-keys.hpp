#pragma once

#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/base64url.hpp"

// Test-only key generation helpers. Each TestKey carries the private PEM (to sign with), the public
// PEM (to verify with), and the public key encoded as a JWK (to exercise the JWK path). Keys are
// generated at runtime with OpenSSL so no large PEM fixtures are embedded.
namespace aeronet::test {

struct TestKey {
  std::string privatePem;
  std::string publicPem;
  std::string jwk;  // public JWK with "kid":"test-kid"
};

namespace detail {

inline std::string BioToString(BIO* bio) {
  char* data = nullptr;
  const long len = ::BIO_get_mem_data(bio, &data);
  return std::string(data, static_cast<std::size_t>(len));
}

inline std::string B64Url(const unsigned char* data, std::size_t len) {
  std::string out;
  out.resize(B64UrlEncodedLen(len));
  B64UrlEncode(std::span<const char>(reinterpret_cast<const char*>(data), len), out.data());
  return out;
}

inline std::string BnB64Url(EVP_PKEY* key, const char* paramName) {
  BIGNUM* bn = nullptr;
  ::EVP_PKEY_get_bn_param(key, paramName, &bn);
  std::string out;
  unsigned char buf[1024];
  const int len = ::BN_bn2bin(bn, buf);
  out = B64Url(buf, static_cast<std::size_t>(len));
  ::BN_free(bn);
  return out;
}

inline std::string RsaJwk(EVP_PKEY* key) {
  return std::string(R"({"kty":"RSA","kid":"test-kid","n":")") + BnB64Url(key, OSSL_PKEY_PARAM_RSA_N) + R"(","e":")" +
         BnB64Url(key, OSSL_PKEY_PARAM_RSA_E) + R"("})";
}

inline std::string EcJwk(EVP_PKEY* key) {
  char group[64] = {};
  std::size_t groupLen = 0;
  ::EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof(group), &groupLen);
  std::string_view grpName(group, groupLen);
  std::string crv;
  std::size_t coord = 0;
  if (grpName == "prime256v1") {
    crv = "P-256";
    coord = 32;
  } else if (grpName == "secp384r1") {
    crv = "P-384";
    coord = 48;
  } else {  // secp521r1
    crv = "P-521";
    coord = 66;
  }
  unsigned char point[256];
  std::size_t pointLen = 0;
  ::EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point), &pointLen);
  // Uncompressed point: 0x04 || X || Y, each coordinate exactly `coord` bytes.
  const std::string x = B64Url(point + 1, coord);
  const std::string y = B64Url(point + 1 + coord, coord);
  return std::string(R"({"kty":"EC","kid":"test-kid","crv":")") + crv + R"(","x":")" + x + R"(","y":")" + y + R"("})";
}

inline std::string OkpJwk(EVP_PKEY* key) {
  unsigned char pub[64];
  std::size_t pubLen = sizeof(pub);
  ::EVP_PKEY_get_raw_public_key(key, pub, &pubLen);
  return std::string(R"({"kty":"OKP","kid":"test-kid","crv":"Ed25519","x":")") + B64Url(pub, pubLen) + R"("})";
}

inline TestKey Export(EVP_PKEY* key, std::string (*jwkFn)(EVP_PKEY*)) {
  TestKey out;
  BIO* priv = ::BIO_new(BIO_s_mem());
  ::PEM_write_bio_PrivateKey(priv, key, nullptr, nullptr, 0, nullptr, nullptr);
  out.privatePem = BioToString(priv);
  ::BIO_free(priv);

  BIO* pub = ::BIO_new(BIO_s_mem());
  ::PEM_write_bio_PUBKEY(pub, key);
  out.publicPem = BioToString(pub);
  ::BIO_free(pub);

  out.jwk = jwkFn(key);
  ::EVP_PKEY_free(key);
  return out;
}

}  // namespace detail

inline TestKey GenerateRsa(unsigned int bits = 2048) { return detail::Export(::EVP_RSA_gen(bits), detail::RsaJwk); }
inline TestKey GenerateEc(const char* curve) { return detail::Export(::EVP_EC_gen(curve), detail::EcJwk); }
inline TestKey GenerateEd25519() {
  return detail::Export(::EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), detail::OkpJwk);
}

}  // namespace aeronet::test
