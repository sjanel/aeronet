#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/jwt-error.hpp"
#include "aeronet/jwt-key.hpp"
#include "aeronet/jwt.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

// A parsed JSON Web Key Set (RFC 7517 §5): the `{"keys":[ ... ]}` document an issuer publishes at
// its JWKS endpoint. Keys whose "kty" is unsupported are skipped rather than failing the whole set.
//
// Pairs naturally with aeronet's HTTP client to fetch and cache an issuer's keys, but parsing is
// transport-agnostic: feed it the JSON however you obtained it.
class Jwks {
 public:
  // Parse a JWK Set document. Never throws: a malformed top-level document leaves the set empty()
  // (logged), and individual unsupported/malformed keys are silently skipped.
  explicit Jwks(std::string_view jwksJson);

  // The key with the given "kid", or nullptr when absent.
  [[nodiscard]] const JwtKey* find(std::string_view kid) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return _keys.size(); }
  [[nodiscard]] bool empty() const noexcept { return _keys.empty(); }

  // Verify `token`, selecting the key by its "kid" header. When the token carries no "kid" and the
  // set holds exactly one key, that key is used. Sets `err` and returns an empty DecodedJwt
  // (valid() == false) on failure (JwtError::KeyMismatch when no key matches the kid).
  [[nodiscard]] DecodedJwt tryDecode(std::string_view token, const JwtVerifyOptions& options, JwtError& err) const;

 private:
  vector<JwtKey> _keys;
};

}  // namespace aeronet
