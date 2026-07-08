#include "aeronet/jwt-key.hpp"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/param_build.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/types.h>

#include <cassert>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/base64url.hpp"
#include "aeronet/jwt-algorithm.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace {

// --- OpenSSL RAII (function-pointer-free, deleter type carries the free function) ---
struct BioDeleter {
  void operator()(BIO* ptr) const noexcept { ::BIO_free(ptr); }
};
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

struct MdCtxDeleter {
  void operator()(EVP_MD_CTX* ptr) const noexcept { ::EVP_MD_CTX_free(ptr); }
};
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

struct PkeyCtxDeleter {
  void operator()(EVP_PKEY_CTX* ptr) const noexcept { ::EVP_PKEY_CTX_free(ptr); }
};
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter>;

struct ParamBldDeleter {
  void operator()(OSSL_PARAM_BLD* ptr) const noexcept { ::OSSL_PARAM_BLD_free(ptr); }
};
using ParamBldPtr = std::unique_ptr<OSSL_PARAM_BLD, ParamBldDeleter>;

struct OsslParamDeleter {
  void operator()(OSSL_PARAM* ptr) const noexcept { ::OSSL_PARAM_free(ptr); }
};
using OsslParamPtr = std::unique_ptr<OSSL_PARAM, OsslParamDeleter>;

struct BnDeleter {
  void operator()(BIGNUM* ptr) const noexcept { ::BN_free(ptr); }
};
using BnPtr = std::unique_ptr<BIGNUM, BnDeleter>;

struct EcdsaSigDeleter {
  void operator()(ECDSA_SIG* ptr) const noexcept { ::ECDSA_SIG_free(ptr); }
};
using EcdsaSigPtr = std::unique_ptr<ECDSA_SIG, EcdsaSigDeleter>;

// --- algorithm classification ---
[[nodiscard]] bool IsRsaPkcs1(JwtAlgorithm alg) noexcept {
  return alg == JwtAlgorithm::RS256 || alg == JwtAlgorithm::RS384 || alg == JwtAlgorithm::RS512;
}
[[nodiscard]] bool IsPss(JwtAlgorithm alg) noexcept {
  return alg == JwtAlgorithm::PS256 || alg == JwtAlgorithm::PS384 || alg == JwtAlgorithm::PS512;
}
[[nodiscard]] bool IsEcdsa(JwtAlgorithm alg) noexcept {
  return alg == JwtAlgorithm::ES256 || alg == JwtAlgorithm::ES384 || alg == JwtAlgorithm::ES512;
}

[[nodiscard]] const EVP_MD* MdFor(JwtAlgorithm alg) noexcept {
  switch (alg) {
    case JwtAlgorithm::HS256:
      [[fallthrough]];
    case JwtAlgorithm::RS256:
      [[fallthrough]];
    case JwtAlgorithm::ES256:
      [[fallthrough]];
    case JwtAlgorithm::PS256:
      return EVP_sha256();
    case JwtAlgorithm::HS384:
      [[fallthrough]];
    case JwtAlgorithm::RS384:
      [[fallthrough]];
    case JwtAlgorithm::ES384:
      [[fallthrough]];
    case JwtAlgorithm::PS384:
      return EVP_sha384();
    case JwtAlgorithm::HS512:
      [[fallthrough]];
    case JwtAlgorithm::RS512:
      [[fallthrough]];
    case JwtAlgorithm::ES512:
      [[fallthrough]];
    case JwtAlgorithm::PS512:
      return EVP_sha512();
    default:
      assert(alg == JwtAlgorithm::EdDSA);
      return nullptr;  // Ed25519 hashes internally — no external digest
  }
}

// Fixed P-curve coordinate length (bytes) for the ECDSA "raw" R||S JWS encoding (RFC 7518 §3.4).
// Only ever called for an EC key signing/verifying with an ES* algorithm.
[[nodiscard]] std::size_t EcCoordLen(JwtAlgorithm alg) noexcept {
  switch (alg) {
    case JwtAlgorithm::ES256:
      return 32;
    case JwtAlgorithm::ES384:
      return 48;
    default:
      assert(alg == JwtAlgorithm::ES512);
      return 66;  // P-521 → ceil(521/8)
  }
}

[[nodiscard]] bool DecodeB64Url(std::string_view in, RawChars& out) {
  assert(out.empty());
  out.ensureAvailableCapacity(B64UrlMaxDecodedLen(in.size()));
  std::size_t outLen = 0;
  if (!B64UrlDecode(in, out.data(), outLen)) {
    return false;
  }
  out.setSize(static_cast<RawChars::size_type>(outLen));
  return true;
}

struct DecodeUrlsResult {
  std::span<const char> decoded1;
  std::span<const char> decoded2;
  bool valid = false;
};

[[nodiscard]] DecodeUrlsResult DecodeB64Urls(std::string_view in1, std::string_view in2, RawChars& out) {
  assert(out.empty());
  DecodeUrlsResult result;
  out.ensureAvailableCapacity(B64UrlMaxDecodedLen(in1.size()) + B64UrlMaxDecodedLen(in2.size()));
  std::size_t outLen;
  if (!B64UrlDecode(in1, out.data(), outLen)) {
    return result;
  }
  result.decoded1 = std::span<const char>(out.data(), outLen);
  if (!B64UrlDecode(in2, out.data() + outLen, outLen)) {
    return result;
  }
  result.decoded2 = std::span<const char>(out.data() + result.decoded1.size(), outLen);
  result.valid = true;
  out.setSize(static_cast<RawChars::size_type>(result.decoded1.size() + result.decoded2.size()));
  return result;
}

// Build an RSA public key from base64url-decoded modulus / exponent.
[[nodiscard]] EVP_PKEY* RsaPublicFromNE(std::span<const char> modulus, std::span<const char> exponent) {
  BnPtr bnN(
      ::BN_bin2bn(reinterpret_cast<const unsigned char*>(modulus.data()), static_cast<int>(modulus.size()), nullptr));
  BnPtr bnE(
      ::BN_bin2bn(reinterpret_cast<const unsigned char*>(exponent.data()), static_cast<int>(exponent.size()), nullptr));
  ParamBldPtr bld(::OSSL_PARAM_BLD_new());
  if (!bnN || !bnE || !bld || ::OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_N, bnN.get()) != 1 ||
      ::OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_E, bnE.get()) != 1) {
    return {};
  }
  OsslParamPtr params(::OSSL_PARAM_BLD_to_param(bld.get()));
  PkeyCtxPtr ctx(::EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr));
  EVP_PKEY* pKey = nullptr;
  if (!params || !ctx || ::EVP_PKEY_fromdata_init(ctx.get()) != 1 ||
      ::EVP_PKEY_fromdata(ctx.get(), &pKey, EVP_PKEY_PUBLIC_KEY, params.get()) != 1) {
    return pKey;
  }
  return pKey;
}

// Build an EC public key from a curve name and base64url-decoded affine coordinates.
[[nodiscard]] EVP_PKEY* EcPublicFromXY(const char* groupName, std::size_t coordLen, std::span<const char> xCoord,
                                       std::span<const char> yCoord) {
  EVP_PKEY* pKey = nullptr;
  if (xCoord.size() != coordLen || yCoord.size() != coordLen) {
    return pKey;
  }
  // Uncompressed point: 0x04 || X || Y.
  RawChars point(1U + (2U * coordLen));
  point.unchecked_push_back('\x04');
  point.unchecked_append(xCoord.data(), coordLen);
  point.unchecked_append(yCoord.data(), coordLen);

  ParamBldPtr bld(::OSSL_PARAM_BLD_new());
  if (!bld || ::OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, groupName, 0) != 1 ||
      ::OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()) != 1) {
    return pKey;
  }
  OsslParamPtr params(::OSSL_PARAM_BLD_to_param(bld.get()));
  PkeyCtxPtr ctx(::EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
  if (!params || !ctx || ::EVP_PKEY_fromdata_init(ctx.get()) != 1 ||
      ::EVP_PKEY_fromdata(ctx.get(), &pKey, EVP_PKEY_PUBLIC_KEY, params.get()) != 1) {
    return pKey;
  }
  return pKey;
}

// Convert an OpenSSL DER ECDSA signature into the fixed-length JWS R||S form.
[[nodiscard]] bool EcdsaDerToRaw(JwtAlgorithm alg, std::span<const char> der, RawChars& out) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(der.data());
  EcdsaSigPtr sig(::d2i_ECDSA_SIG(nullptr, &ptr, static_cast<long>(der.size())));
  if (!sig) {
    return false;
  }
  const BIGNUM* bnR = nullptr;
  const BIGNUM* bnS = nullptr;
  ::ECDSA_SIG_get0(sig.get(), &bnR, &bnS);
  const auto coord = static_cast<int>(EcCoordLen(alg));
  const auto base = out.size();
  out.ensureAvailableCapacity(2 * coord);
  out.setSize(base + static_cast<RawChars::size_type>(2 * coord));
  auto* dst = reinterpret_cast<unsigned char*>(out.data()) + base;
  return ::BN_bn2binpad(bnR, dst, coord) == coord && ::BN_bn2binpad(bnS, dst + coord, coord) == coord;
}

// Convert a fixed-length JWS R||S ECDSA signature into DER for EVP verification.
[[nodiscard]] bool EcdsaRawToDer(JwtAlgorithm alg, std::string_view raw, RawChars& out) {
  const std::size_t coord = EcCoordLen(alg);
  if (raw.size() != 2 * coord) {
    return false;
  }
  BnPtr bnR(::BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()), static_cast<int>(coord), nullptr));
  BnPtr bnS(::BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()) + coord, static_cast<int>(coord), nullptr));
  EcdsaSigPtr sig(::ECDSA_SIG_new());
  if (!bnR || !bnS || !sig || ::ECDSA_SIG_set0(sig.get(), bnR.get(), bnS.get()) != 1) {
    return false;
  }
  bnR.release();  // NOLINT(bugprone-unused-return-value) ownership transferred to sig
  bnS.release();  // NOLINT(bugprone-unused-return-value) ownership transferred to sig
  unsigned char* der = nullptr;
  const int derLen = ::i2d_ECDSA_SIG(sig.get(), &der);
  if (derLen <= 0) {
    return false;
  }
  out.append(reinterpret_cast<const char*>(der), static_cast<RawChars::size_type>(derLen));
  ::OPENSSL_free(der);
  return true;
}

}  // namespace

namespace {

[[nodiscard]] bool FamilyOf(EVP_PKEY* pkey, JwtKey::KeyFamily& out) {
  switch (::EVP_PKEY_base_id(pkey)) {
    case EVP_PKEY_RSA:
      [[fallthrough]];
    case EVP_PKEY_RSA_PSS:
      out = JwtKey::KeyFamily::Rsa;
      return true;
    case EVP_PKEY_EC:
      out = JwtKey::KeyFamily::Ec;
      return true;
    case EVP_PKEY_ED25519:
      out = JwtKey::KeyFamily::Ed25519;
      return true;
    default:
      return false;  // unsupported key type (only RSA, EC and Ed25519 are usable for JWS)
  }
}

}  // namespace

JwtKey::JwtKey(JwtKey&& other) noexcept
    : family(std::exchange(other.family, KeyFamily::Empty)),
      secret(std::move(other.secret)),
      kid(std::move(other.kid)),
      pKey(std::exchange(other.pKey, nullptr)) {}

JwtKey& JwtKey::operator=(JwtKey&& other) noexcept {
  if (this != &other) {
    family = std::exchange(other.family, KeyFamily::Empty);
    secret = std::move(other.secret);
    kid = std::move(other.kid);
    pKey = std::exchange(other.pKey, nullptr);
  }
  return *this;
}

JwtKey::~JwtKey() { ::EVP_PKEY_free(static_cast<EVP_PKEY*>(pKey)); }

JwtKey JwtKey::Hmac(std::string_view secret) {
  if (secret.empty()) {
    log::warn("JWT: ignoring empty HMAC secret");
    return {};
  }
  JwtKey ret;
  ret.family = KeyFamily::Hmac;
  ret.secret.assign(secret);
  return ret;
}

JwtKey JwtKey::FromPem(std::string_view pem) {
  BioPtr bio(::BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
  EVP_PKEY* pKey = bio ? ::PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) : nullptr;
  JwtKey ret;
  if (pKey == nullptr && bio) {
    // Not a private key — retry as a SubjectPublicKeyInfo public key.
    bio.reset(::BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    pKey = bio ? ::PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr) : nullptr;
  }
  if (pKey == nullptr) {
    log::warn("JWT: failed to parse PEM key");
    return ret;
  }
  ret.pKey = static_cast<void*>(pKey);
  if (!FamilyOf(pKey, ret.family)) {
    log::warn("JWT: unsupported PEM key type (only RSA, EC and Ed25519 are supported)");
    return ret;
  }
  return ret;
}

JwtKey JwtKey::FromJwk(std::string_view jwkJson) {
  jwt_detail::JwkDocView doc;
  // Ignore unknown JWK members; the caller's view may not be null-terminated.
  if (glz::read<glz::opts{.null_terminated = false, .error_on_unknown_keys = false}>(doc, jwkJson)) {
    log::warn("JWT: malformed JWK JSON");
    return {};
  }
  return FromJwkDoc(doc);
}

JwtKey JwtKey::FromJwkDoc(const jwt_detail::JwkDocView& doc) {
  JwtKey ret;
  if (doc.kty == "oct") {
    if (!DecodeB64Url(doc.k, ret.secret) || ret.secret.empty()) {
      log::warn("JWT: invalid or empty oct JWK key");
      return ret;
    }
    ret.family = KeyFamily::Hmac;
  } else if (doc.kty == "RSA") {
    RawChars buf;  // modulus and exponent will be decoded into this single buffer
    const auto decodeResult = DecodeB64Urls(doc.n, doc.e, buf);
    if (!decodeResult.valid) {
      log::warn("JWT: invalid base64url in RSA JWK");
      return ret;
    }
    ret.pKey = RsaPublicFromNE(decodeResult.decoded1, decodeResult.decoded2);
    if (ret.pKey == nullptr) {
      log::warn("JWT: failed to build RSA public key from JWK");
      return ret;
    }
    ret.family = KeyFamily::Rsa;
  } else if (doc.kty == "EC") {
    RawChars buf;  // x and y coordinates will be decoded into this single buffer
    const auto decodeResult = DecodeB64Urls(doc.x, doc.y, buf);
    if (!decodeResult.valid) {
      log::warn("JWT: invalid base64url in EC JWK");
      return ret;
    }
    std::size_t coord;
    const char* group;
    if (doc.crv == "P-256") {
      group = "P-256";
      coord = 32;
    } else if (doc.crv == "P-384") {
      group = "P-384";
      coord = 48;
    } else if (doc.crv == "P-521") {
      group = "P-521";
      coord = 66;
    } else {
      log::warn("JWT: unsupported EC curve in JWK");
      return ret;
    }
    ret.pKey = EcPublicFromXY(group, coord, decodeResult.decoded1, decodeResult.decoded2);
    if (ret.pKey == nullptr) {
      log::warn("JWT: failed to build EC public key from JWK");
      return ret;
    }
    ret.family = KeyFamily::Ec;
  } else if (doc.kty == "OKP") {
    RawChars pub;
    if (doc.crv != "Ed25519" || !DecodeB64Url(doc.x, pub)) {
      log::warn("JWT: unsupported or invalid OKP JWK (only Ed25519)");
      return ret;
    }
    EVP_PKEY* pKey = ::EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                   reinterpret_cast<const unsigned char*>(pub.data()), pub.size());
    if (pKey == nullptr) {
      log::warn("JWT: failed to build Ed25519 public key from JWK");
      return ret;
    }
    ret.pKey = static_cast<void*>(pKey);
    ret.family = KeyFamily::Ed25519;
  } else {
    log::warn("JWT: unsupported JWK key type");
    return ret;
  }
  ret.kid.assign(doc.kid);
  return ret;
}

bool JwtKey::matchesFamily(JwtAlgorithm alg) const noexcept {
  switch (family) {
    case KeyFamily::Hmac:
      return IsHmac(alg);
    case KeyFamily::Rsa:
      return IsRsaPkcs1(alg) || IsPss(alg);
    case KeyFamily::Ec:
      return IsEcdsa(alg);
    default:
      assert(family == KeyFamily::Ed25519);
      return alg == JwtAlgorithm::EdDSA;
  }
}

bool JwtKey::sign(JwtAlgorithm alg, std::string_view signingInput, RawChars& out) const {
  if (!matchesFamily(alg)) {
    return false;
  }
  const auto* msg = reinterpret_cast<const unsigned char*>(signingInput.data());
  if (IsHmac(alg)) {
    out.ensureAvailableCapacity(EVP_MAX_MD_SIZE);
    unsigned int macLen = 0;
    if (::HMAC(MdFor(alg), secret.data(), static_cast<int>(secret.size()), msg, signingInput.size(),
               reinterpret_cast<unsigned char*>(out.data() + out.size()), &macLen) == nullptr) {
      return false;
    }
    out.addSize(static_cast<RawChars::size_type>(macLen));
    return true;
  }

  MdCtxPtr ctx(::EVP_MD_CTX_new());
  if (!ctx) {
    return false;
  }
  EVP_PKEY_CTX* pctx = nullptr;
  if (::EVP_DigestSignInit(ctx.get(), &pctx, MdFor(alg), nullptr, static_cast<EVP_PKEY*>(pKey)) != 1) {
    return false;
  }
  if (IsPss(alg) && (::EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
                     ::EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1 ||
                     ::EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, MdFor(alg)) != 1)) {
    return false;
  }
  std::size_t sigLen = 0;
  if (::EVP_DigestSign(ctx.get(), nullptr, &sigLen, msg, signingInput.size()) != 1) {
    return false;
  }
  RawChars sig(sigLen);
  if (::EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char*>(sig.data()), &sigLen, msg, signingInput.size()) !=
      1) {
    return false;
  }
  sig.setSize(static_cast<RawChars::size_type>(sigLen));
  if (family == KeyFamily::Ec) {
    return EcdsaDerToRaw(alg, sig, out);
  }
  out.append(sig);
  return true;
}

bool JwtKey::verify(JwtAlgorithm alg, std::string_view signingInput, std::string_view signature) const {
  if (!matchesFamily(alg)) {
    return false;
  }
  const auto* msg = reinterpret_cast<const unsigned char*>(signingInput.data());
  if (IsHmac(alg)) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int macLen = 0;
    if (::HMAC(MdFor(alg), secret.data(), static_cast<int>(secret.size()), msg, signingInput.size(), mac, &macLen) ==
        nullptr) {
      return false;
    }
    return signature.size() == macLen && ::CRYPTO_memcmp(mac, signature.data(), macLen) == 0;
  }

  MdCtxPtr ctx(::EVP_MD_CTX_new());
  if (!ctx) {
    return false;
  }
  EVP_PKEY_CTX* pctx = nullptr;
  if (::EVP_DigestVerifyInit(ctx.get(), &pctx, MdFor(alg), nullptr, static_cast<EVP_PKEY*>(pKey)) != 1) {
    return false;
  }
  if (IsPss(alg) && (::EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
                     ::EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1 ||
                     ::EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, MdFor(alg)) != 1)) {
    return false;
  }
  RawChars der;
  if (family == KeyFamily::Ec) {
    if (!EcdsaRawToDer(alg, signature, der)) {
      return false;
    }
    signature = der;
  }
  return ::EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(signature.data()), signature.size(), msg,
                            signingInput.size()) == 1;
}

}  // namespace aeronet
