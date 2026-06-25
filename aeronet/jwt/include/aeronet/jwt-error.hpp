#pragma once

#include <cassert>
#include <cstdint>
#include <string_view>

namespace aeronet {

// Why a JWT failed to verify. `None` is the success sentinel returned by Jwt::tryDecode.
enum class JwtError : std::uint8_t {
  None,               // success
  Malformed,          // not three base64url segments, bad base64url, or invalid header/claims JSON
  UnsupportedAlg,     // "alg" header missing, unknown, or the rejected "none"
  AlgNotAllowed,      // "alg" not in JwtVerifyOptions::allowedAlgorithms
  KeyMismatch,        // key family incompatible with "alg" (e.g. HMAC key for an RS256 token)
  InvalidSignature,   // signature did not verify against the key
  Expired,            // "exp" is in the past (beyond leeway)
  NotYetValid,        // "nbf" is in the future (beyond leeway)
  MissingExpiration,  // JwtVerifyOptions::requireExpiration set but "exp" absent
  IssuerMismatch,     // "iss" did not match the expected issuer
  AudienceMismatch,   // expected audience not present in "aud"
  SubjectMismatch,    // "sub" did not match the expected subject
};

// Human-readable description, e.g. for logging or exception messages.
[[nodiscard]] constexpr std::string_view ToString(JwtError err) noexcept {
  switch (err) {
    case JwtError::None:
      return "ok";
    case JwtError::Malformed:
      return "malformed token";
    case JwtError::UnsupportedAlg:
      return "unsupported or unsecured algorithm";
    case JwtError::AlgNotAllowed:
      return "algorithm not in allowed set";
    case JwtError::KeyMismatch:
      return "key incompatible with algorithm";
    case JwtError::InvalidSignature:
      return "invalid signature";
    case JwtError::Expired:
      return "token expired";
    case JwtError::NotYetValid:
      return "token not yet valid";
    case JwtError::MissingExpiration:
      return "missing required expiration";
    case JwtError::IssuerMismatch:
      return "issuer mismatch";
    case JwtError::AudienceMismatch:
      return "audience mismatch";
    default:
      assert(err == JwtError::SubjectMismatch);
      return "subject mismatch";
  }
}

}  // namespace aeronet
