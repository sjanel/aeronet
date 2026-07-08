#include "aeronet/jwks.hpp"

#include <algorithm>
#include <glaze/glaze.hpp>
#include <string_view>

#include "aeronet/jwt-error.hpp"
#include "aeronet/jwt-key.hpp"
#include "aeronet/jwt.hpp"
#include "aeronet/log.hpp"

namespace aeronet {

Jwks::Jwks(std::string_view jwksJson) {
  glz::generic doc;
  // Caller-supplied view may not be null-terminated; honour the end pointer strictly.
  if (glz::read<glz::opts{.null_terminated = false}>(doc, jwksJson) || !doc.is_object() || !doc.contains("keys")) {
    log::warn("JWT: malformed JWKS document");
    return;
  }
  const auto& keys = doc.at("keys");
  if (!keys.is_array()) {
    log::warn("JWT: JWKS \"keys\" must be an array");
    return;
  }
  for (const auto& node : keys.get_array()) {
    const auto keyJson = glz::write_json(node);
    if (!keyJson) {
      continue;
    }
    // Unsupported / malformed individual keys come back invalid — skip them, keep the rest usable.
    if (!_keys.emplace_back(JwtKey::FromJwk(*keyJson)).valid()) {
      _keys.pop_back();
    }
  }
}

const JwtKey* Jwks::find(std::string_view kid) const noexcept {
  const auto it = std::ranges::find_if(_keys, [kid](const JwtKey& key) { return key.keyId() == kid; });
  if (it == _keys.end()) {
    return nullptr;
  }
  return &*it;
}

DecodedJwt Jwks::tryDecode(std::string_view token, const JwtVerifyOptions& options, JwtError& err) const {
  // Delegate to the shared decode core, picking the key from the JOSE header "kid" it parses for us
  // (so the header is base64-decoded and JSON-parsed once, not twice). A "kid" hit — or a missing
  // "kid" with an unambiguous single-key set — selects the key; anything else yields nullptr, which
  // the core reports as KeyMismatch.
  return Jwt::tryDecodeImpl(
      token,
      [](std::string_view kid, const void* ctx) -> const JwtKey* {
        const auto& self = *static_cast<const Jwks*>(ctx);
        if (!kid.empty()) {
          return self.find(kid);
        }
        return self._keys.size() == 1 ? &self._keys.front() : nullptr;
      },
      this, options, err);
}

}  // namespace aeronet
