#pragma once

#include <cassert>
#include <cstdint>
#include <string_view>

namespace aeronet {

// JWS signature algorithms supported by aeronet (RFC 7518 §3). aeronet implements the JWS
// (signature) profile of JWT; JWE (encryption) is intentionally out of scope.
//
// The unsecured "none" algorithm (RFC 7518 §3.6) is deliberately NOT modelled: it is rejected at
// parse time so a stripped-signature token can never be accepted.
enum class JwtAlgorithm : std::uint8_t {
  HS256,  // HMAC using SHA-256        (symmetric, "Required" by RFC 7518)
  HS384,  // HMAC using SHA-384
  HS512,  // HMAC using SHA-512
  RS256,  // RSASSA-PKCS1-v1_5 + SHA-256 ("Recommended")
  RS384,  // RSASSA-PKCS1-v1_5 + SHA-384
  RS512,  // RSASSA-PKCS1-v1_5 + SHA-512
  ES256,  // ECDSA P-256 + SHA-256       ("Recommended+")
  ES384,  // ECDSA P-384 + SHA-384
  ES512,  // ECDSA P-521 + SHA-512
  PS256,  // RSASSA-PSS + SHA-256
  PS384,  // RSASSA-PSS + SHA-384
  PS512,  // RSASSA-PSS + SHA-512
  EdDSA,  // EdDSA (Ed25519, RFC 8037)
};

// Canonical JOSE "alg" header value for `alg`, e.g. "HS256".
[[nodiscard]] constexpr std::string_view ToString(JwtAlgorithm alg) noexcept {
  switch (alg) {
    case JwtAlgorithm::HS256:
      return "HS256";
    case JwtAlgorithm::HS384:
      return "HS384";
    case JwtAlgorithm::HS512:
      return "HS512";
    case JwtAlgorithm::RS256:
      return "RS256";
    case JwtAlgorithm::RS384:
      return "RS384";
    case JwtAlgorithm::RS512:
      return "RS512";
    case JwtAlgorithm::ES256:
      return "ES256";
    case JwtAlgorithm::ES384:
      return "ES384";
    case JwtAlgorithm::ES512:
      return "ES512";
    case JwtAlgorithm::PS256:
      return "PS256";
    case JwtAlgorithm::PS384:
      return "PS384";
    case JwtAlgorithm::PS512:
      return "PS512";
    default:
      assert(alg == JwtAlgorithm::EdDSA);
      return "EdDSA";
  }
}

// Parse a JOSE "alg" header value into `out`. Returns false for unknown algorithms and for the
// unsecured "none" value (which aeronet refuses by design).
[[nodiscard]] constexpr bool FromString(std::string_view str, JwtAlgorithm& out) noexcept {
  for (auto candidate :
       {JwtAlgorithm::HS256, JwtAlgorithm::HS384, JwtAlgorithm::HS512, JwtAlgorithm::RS256, JwtAlgorithm::RS384,
        JwtAlgorithm::RS512, JwtAlgorithm::ES256, JwtAlgorithm::ES384, JwtAlgorithm::ES512, JwtAlgorithm::PS256,
        JwtAlgorithm::PS384, JwtAlgorithm::PS512, JwtAlgorithm::EdDSA}) {
    if (ToString(candidate) == str) {
      out = candidate;
      return true;
    }
  }
  return false;
}

// True for the HMAC family (symmetric shared secret); false for the asymmetric families.
[[nodiscard]] constexpr bool IsHmac(JwtAlgorithm alg) noexcept {
  return alg == JwtAlgorithm::HS256 || alg == JwtAlgorithm::HS384 || alg == JwtAlgorithm::HS512;
}

}  // namespace aeronet
