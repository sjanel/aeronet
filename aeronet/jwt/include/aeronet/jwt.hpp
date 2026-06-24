#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "aeronet/dynamic-concatenated-strings.hpp"
#include "aeronet/jwt-algorithm-set.hpp"
#include "aeronet/jwt-algorithm.hpp"
#include "aeronet/jwt-error.hpp"
#include "aeronet/jwt-key.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/static-concatenated-strings.hpp"

namespace aeronet {

namespace jwt_detail {
// '\0' separator for the audience list — an audience is a JSON string and effectively never carries
// a NUL byte (one that did would be rejected as malformed at decode time).
inline constexpr char kAudienceSep[] = "";
}  // namespace jwt_detail

// A verified JWT: its JOSE header and decoded claim set. Returned by Jwt::tryDecode only after the
// signature and all enabled validations have passed; on any failure tryDecode returns the empty
// state instead (valid() == false), so callers test the result directly rather than unwrapping a
// std::optional.
//
// Registered claims (RFC 7519 §4.1) are surfaced through typed accessors. Absence is encoded by the
// natural empty state (an empty string_view, or a 0 NumericDate) rather than std::optional — a
// registered claim is never meaningfully empty, and no real token carries exp/nbf/iat == 0.
//
// The five header/registered string claims live in a single StaticConcatenatedStrings buffer (one
// allocation, O(1) indexed views) and the audiences in a DynamicConcatenatedStrings (one allocation,
// in-line scan) — both far lighter and trivially relocatable, unlike a pile of std::string fields.
// The verbatim header/payload JSON stay available via headerJson() / payloadJson() so callers can
// deserialize application-specific claims (e.g. with glaze) without this type imposing a schema.
class DecodedJwt {
 public:
  using Audiences = DynamicConcatenatedStrings<jwt_detail::kAudienceSep, std::uint32_t>;

  // True for a token Jwt::tryDecode actually verified; false for the empty state it returns on any
  // failure. A successfully decoded token always carries a non-empty payload (a JSON object), so an
  // empty payload buffer unambiguously marks the failure sentinel.
  [[nodiscard]] bool valid() const noexcept { return !_payloadJson.empty(); }
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

  [[nodiscard]] JwtAlgorithm algorithm() const noexcept { return _alg; }

  // JOSE header members (empty view == absent).
  [[nodiscard]] std::string_view keyId() const noexcept { return _claims[kKeyId]; }
  [[nodiscard]] std::string_view type() const noexcept { return _claims[kType]; }

  // Registered string claims (empty view == absent).
  [[nodiscard]] std::string_view issuer() const noexcept { return _claims[kIssuer]; }
  [[nodiscard]] std::string_view subject() const noexcept { return _claims[kSubject]; }
  [[nodiscard]] std::string_view jwtId() const noexcept { return _claims[kJwtId]; }

  // "aud" may be a single string or an array; both are normalised to this list (empty == none).
  [[nodiscard]] const Audiences& audiences() const noexcept { return _aud; }
  [[nodiscard]] bool hasAudience(std::string_view audience) const noexcept { return _aud.contains(audience); }

  // Registered NumericDate claims (RFC 7519 §2): seconds since the Unix epoch, 0 == absent.
  [[nodiscard]] int64_t expiresAt() const noexcept { return _exp; }
  [[nodiscard]] int64_t notBefore() const noexcept { return _nbf; }
  [[nodiscard]] int64_t issuedAt() const noexcept { return _iat; }

  // Raw decoded JSON, for verbatim access / custom deserialization. Views are valid for the
  // lifetime of this object.
  [[nodiscard]] std::string_view headerJson() const noexcept { return _headerJson; }
  [[nodiscard]] std::string_view payloadJson() const noexcept { return _payloadJson; }

 private:
  friend class Jwt;

  // Indices into _claims; kClaimCount is the part count.
  enum : std::uint8_t { kKeyId, kType, kIssuer, kSubject, kJwtId, kClaimCount };

  StaticConcatenatedStrings<kClaimCount, std::uint32_t> _claims;
  Audiences _aud;
  RawChars32 _headerJson;
  RawChars32 _payloadJson;
  int64_t _exp{};
  int64_t _nbf{};
  int64_t _iat{};
  JwtAlgorithm _alg{};
};

// Controls which validations Jwt::tryDecode applies on top of the mandatory signature check.
// Every check beyond the signature is opt-in, except the unsecured "none" algorithm which is always
// rejected.
struct JwtVerifyOptions {
  // When non-empty, the token "alg" must be a member (mitigates algorithm-substitution attacks).
  // When empty, any supported algorithm whose family matches the key is accepted.
  JwtAlgorithmSet allowedAlgorithms;

  // When set, the corresponding claim must be present and equal / contained.
  std::string_view issuer;
  std::string_view audience;
  std::string_view subject;

  // Clock-skew tolerance applied to the exp / nbf comparisons.
  std::chrono::seconds leeway{0};

  // Reject the token when "exp" is absent.
  bool requireExpiration{false};
  // Enable the "exp" (expiry) and "nbf" (not-before) temporal checks.
  bool validateExpiration{true};
  bool validateNotBefore{true};

  // Reference time for temporal checks. Left at its default (epoch) it means "use system_clock::now()".
  std::chrono::system_clock::time_point clock;
};

// JWS-profile JWT encoding (signing) and decoding (verification). Stateless: every method is static.
//
// The whole module is exception-free: failures are reported via return values (an invalid JwtKey, an
// empty token, or a JwtError), never thrown. The reason is also logged.
class Jwt {
 public:
  // Sign `claimsJson` (a JSON object — the payload) with `key` using `alg`, producing the compact
  // JWS serialization "base64url(header).base64url(payload).base64url(signature)".
  // When `keyId` is non-empty it is emitted as the JOSE header "kid".
  // Returns an empty string on failure (invalid key, key/algorithm mismatch, or crypto error) — a
  // successful token is never empty.
  [[nodiscard]] static std::string encode(std::string_view claimsJson, const JwtKey& key, JwtAlgorithm alg,
                                          std::string_view keyId = {});

  // Verify `token` against `key` with `options`. On success returns the decoded token (valid() ==
  // true) and sets `err` to JwtError::None. On any failure returns an empty DecodedJwt (valid() ==
  // false) and sets `err` to the reason.
  [[nodiscard]] static DecodedJwt tryDecode(std::string_view token, const JwtKey& key, const JwtVerifyOptions& options,
                                            JwtError& err);

 private:
  friend class Jwks;

  // Selects the verifying key from the JOSE header "kid" (empty when the header carries none); returns
  // nullptr when no key matches (→ JwtError::KeyMismatch). `ctx` is the opaque pointer threaded
  // through tryDecodeImpl — a `const JwtKey*` for the single-key path, the Jwks for a key set.
  using KeyResolver = const JwtKey* (*)(std::string_view kid, const void* ctx);

  // Shared decode core. Splits the token and decodes/parses the JOSE header exactly once, then picks
  // the key via `resolveKey` — so Jwks::tryDecode no longer base64-decodes and JSON-parses the header
  // a second time just to read "kid". Otherwise identical to the public tryDecode.
  [[nodiscard]] static DecodedJwt tryDecodeImpl(std::string_view token, KeyResolver resolveKey, const void* ctx,
                                                const JwtVerifyOptions& options, JwtError& err);
};

}  // namespace aeronet
