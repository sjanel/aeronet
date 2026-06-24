#pragma once

#include <memory>
#include <string_view>
#include <type_traits>

#include "aeronet/jwt-algorithm.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace jwt_detail {
struct JwkDocView {
  std::string_view kty;
  std::string_view kid;
  std::string_view n;    // RSA modulus
  std::string_view e;    // RSA exponent
  std::string_view crv;  // EC / OKP curve
  std::string_view x;    // EC x / OKP public key
  std::string_view y;    // EC y
  std::string_view k;    // oct (HMAC) key
};
}  // namespace jwt_detail

class Jwt;
class Jwks;

// A signing / verification key for a JWS-profile JWT.
//
// Three flavours, built through the static factories:
//   * Hmac(secret)    — symmetric shared secret for the HS256/384/512 family.
//   * FromPem(pem)    — an RSA / EC / Ed25519 key in PEM form. A private key can both sign and
//                       verify; a public key can only verify.
//   * FromJwk(json)   — a single JSON Web Key (RFC 7517): "oct" (HMAC), "RSA", "EC", or "OKP".
//
// OpenSSL is hidden behind a pImpl so this public header pulls in no crypto headers. Keys are
// created rarely and live across many requests, so the single owning allocation is irrelevant.
//
// Move-only (it owns key material). The factories never throw: on malformed input they log the
// reason and return an invalid key (valid() == false), so callers check the result rather than
// catching exceptions.
class JwtKey {
 public:
  // An empty, invalid key (valid() == false). Useful as a not-found sentinel.
  JwtKey() noexcept;

  JwtKey(const JwtKey&) = delete;
  JwtKey& operator=(const JwtKey&) = delete;
  JwtKey(JwtKey&&) noexcept;
  JwtKey& operator=(JwtKey&&) noexcept;
  ~JwtKey();

  // Symmetric secret for HS256/HS384/HS512. The bytes are copied.
  [[nodiscard]] static JwtKey Hmac(std::string_view secret);

  // Load an RSA / EC / Ed25519 key from PEM (auto-detecting public vs private).
  [[nodiscard]] static JwtKey FromPem(std::string_view pem);

  // Load a single JSON Web Key object (RFC 7517).
  [[nodiscard]] static JwtKey FromJwk(std::string_view jwkJson);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

  // The "kid" carried by a JWK, or empty for HMAC/PEM keys (or a JWK without one).
  [[nodiscard]] std::string_view keyId() const noexcept;

  using trivially_relocatable = std::true_type;

 private:
  friend class Jwt;
  friend class Jwks;

  struct Impl;
  explicit JwtKey(std::unique_ptr<Impl> impl) noexcept;

  // True when the key's cryptographic family can be used with `alg` (HMAC↔HS*, RSA↔RS*/PS*,
  // EC↔ES*, Ed25519↔EdDSA). A negative answer maps to JwtError::KeyMismatch.
  [[nodiscard]] bool matchesFamily(JwtAlgorithm alg) const noexcept;

  // Sign `signingInput` (the "header.payload" ASCII) with `alg`, appending the raw signature bytes
  // to `out`. Returns false on a key/alg mismatch or a low-level crypto failure.
  [[nodiscard]] bool sign(JwtAlgorithm alg, std::string_view signingInput, RawChars& out) const;

  // Verify `signature` (raw bytes) over `signingInput` with `alg`. Constant-time for HMAC.
  [[nodiscard]] bool verify(JwtAlgorithm alg, std::string_view signingInput, std::string_view signature) const;

  // Build a key from a pre-parsed JWK object view (used by Jwks to avoid a second parse).
  [[nodiscard]] static JwtKey FromJwkDoc(const jwt_detail::JwkDocView& doc);

  std::unique_ptr<Impl> _impl;
};

}  // namespace aeronet
