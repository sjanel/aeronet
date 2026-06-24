#include "aeronet/jwt.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <glaze/glaze.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/base64url.hpp"
#include "aeronet/jwt-algorithm-set.hpp"
#include "aeronet/jwt-algorithm.hpp"
#include "aeronet/jwt-error.hpp"
#include "aeronet/jwt-key.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace {

struct JwtJoseHeader {
  glz::raw_json_view alg;
  glz::raw_json_view typ;
  glz::raw_json_view kid;
  glz::raw_json_view crit;
};

}  // namespace

}  // namespace aeronet

template <>
struct glz::meta<aeronet::JwtJoseHeader> {
  using T = aeronet::JwtJoseHeader;
  static constexpr auto value = glz::object("alg", &T::alg, "typ", &T::typ, "kid", &T::kid, "crit", &T::crit);
};

namespace aeronet {

namespace {

enum class JsonStringState : std::uint8_t { Absent, String, PresentNonString };

[[nodiscard]] JsonStringState ReadOptionalJsonString(const glz::raw_json_view& raw, std::string& out) {
  if (raw.str.empty()) {
    out.clear();
    return JsonStringState::Absent;
  }
  if (glz::read<glz::opts{.null_terminated = false}>(out, raw.str)) {
    out.clear();
    return JsonStringState::PresentNonString;
  }
  return JsonStringState::String;
}

// Append base64url(src) to dst (single resize, no intermediate allocation, and resize_and_overwrite
// skips zero-initialising the freshly grown bytes that B64UrlEncode overwrites anyway).
void AppendB64Url(std::string& dst, const char* pSrc, std::size_t srcLen) {
  const auto base = dst.size();
  const auto encLen = B64UrlEncodedLen(srcLen);
  dst.resize_and_overwrite(base + encLen, [&](char* buf, std::size_t newSize) {
    B64UrlEncode(std::span<const char>(pSrc, srcLen), buf + base);
    return newSize;
  });
}

constexpr std::size_t ComputeJsonStringSize(std::string_view str) {
  std::size_t size = 2;  // quotes
  for (char ch : str) {
    switch (ch) {
      case '"':
        [[fallthrough]];
      case '\\':
        [[fallthrough]];
      case '\n':
        [[fallthrough]];
      case '\r':
        [[fallthrough]];
      case '\t':
        size += 2;
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          size += 6;  // \u00XX
        } else {
          ++size;
        }
    }
  }
  return size;
}

// Append a quoted, minimally-escaped JSON string. Only the JOSE "kid" we emit flows through here.
char* AppendJsonString(std::string_view str, char* pInsertPtr) {
  *pInsertPtr++ = '"';
  for (char ch : str) {
    switch (ch) {
      case '"':
        std::memcpy(pInsertPtr, "\\\"", 2);
        pInsertPtr += 2;
        break;
      case '\\':
        std::memcpy(pInsertPtr, "\\\\", 2);
        pInsertPtr += 2;
        break;
      case '\n':
        std::memcpy(pInsertPtr, "\\n", 2);
        pInsertPtr += 2;
        break;
      case '\r':
        std::memcpy(pInsertPtr, "\\r", 2);
        pInsertPtr += 2;
        break;
      case '\t':
        std::memcpy(pInsertPtr, "\\t", 2);
        pInsertPtr += 2;
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          const char esc[] = {'\\',
                              'u',
                              '0',
                              '0',
                              kHex[(static_cast<unsigned char>(ch) >> 4) & 0xF],
                              kHex[static_cast<unsigned char>(ch) & 0xF]};
          pInsertPtr = Append(esc, sizeof(esc), pInsertPtr);
        } else {
          *pInsertPtr++ = ch;
        }
    }
  }
  *pInsertPtr++ = '"';
  return pInsertPtr;
}

// Decode a base64url segment into `out` (RawChars or RawChars32). The JWT segments are always small,
// so RawChars32 is enough for the stored header/payload.
template <class Buf>
[[nodiscard]] bool DecodeSegment(std::string_view b64, Buf& out) {
  out.clear();
  out.ensureAvailableCapacity(B64UrlMaxDecodedLen(b64.size()));
  std::size_t outLen = 0;
  if (!B64UrlDecode(b64, out.data(), outLen)) {
    return false;
  }
  out.setSize(static_cast<typename Buf::size_type>(outLen));
  return true;
}

// Read an optional string member into part `idx` of a StaticConcatenatedStrings (absent leaves the
// part empty). Returns false (malformed) when present but not a string.
template <class ClaimStore>
[[nodiscard]] bool ReadClaim(const glz::generic& obj, std::string_view key, ClaimStore& claims, std::uint32_t idx) {
  if (!obj.contains(key)) {
    return true;
  }
  const auto& node = obj.at(key);
  if (!node.is_string()) {
    return false;
  }
  claims.set(idx, node.get_string());
  return true;
}

// Read an optional NumericDate (seconds since epoch; absent leaves `out` at 0). Present-but-not-a-
// number is malformed.
[[nodiscard]] bool ReadTime(const glz::generic& obj, std::string_view key, int64_t& out) {
  if (!obj.contains(key)) {
    return true;
  }
  const auto& node = obj.at(key);
  if (!node.is_number()) {
    return false;
  }
  out = static_cast<int64_t>(node.get<double>());
  return true;
}

// Append an audience to the list, rejecting the pathological case of a NUL byte (the list separator)
// embedded in the value.
[[nodiscard]] bool AppendAudience(DecodedJwt::Audiences& aud, std::string_view value) {
  if (value.contains('\0')) {
    return false;
  }
  aud.append(value);
  return true;
}

}  // namespace

std::string Jwt::encode(std::string_view claimsJson, const JwtKey& key, JwtAlgorithm alg, std::string_view keyId) {
  // Built in place: the compact serialization is the returned string, so there is no final copy.
  std::string token;
  if (!key.valid() || !key.matchesFamily(alg)) {
    log::warn("JWT: cannot encode, key incompatible with algorithm {}", ToString(alg));
    return token;
  }

  // Build the JOSE header JSON: {"alg":"<alg>","typ":"JWT"[,"kid":"<kid>"]}.

  static constexpr std::string_view kAlg = R"({"alg":")";
  const auto algStr = ToString(alg);
  static constexpr std::string_view kType = R"(","typ":"JWT")";
  static constexpr std::string_view kKid = R"(,"kid":)";
  const auto jsonStrSize = ComputeJsonStringSize(keyId);

  RawChars buf(kAlg.size() + algStr.size() + kType.size() + (keyId.empty() ? 0U : (kKid.size() + jsonStrSize)) + 1U);
  char* pInsertPtr = buf.data();
  pInsertPtr = Append(kAlg, pInsertPtr);
  pInsertPtr = Append(algStr, pInsertPtr);
  pInsertPtr = Append(kType, pInsertPtr);
  if (!keyId.empty()) {
    pInsertPtr = Append(kKid, pInsertPtr);
    pInsertPtr = AppendJsonString(keyId, pInsertPtr);
  }
  *pInsertPtr++ = '}';
  buf.setSize(static_cast<std::size_t>(pInsertPtr - buf.data()));

  // Reserve the "header.payload" signing input in a single allocation (the signature, whose size we
  // only know after signing, is appended afterwards).
  token.reserve(B64UrlEncodedLen(buf.size()) + 1U + B64UrlEncodedLen(claimsJson.size()));
  AppendB64Url(token, buf.data(), buf.size());
  token.push_back('.');
  AppendB64Url(token, claimsJson.data(), claimsJson.size());

  buf.clear();
  if (!key.sign(alg, token, buf)) {
    log::warn("JWT: signing failed for algorithm {}", ToString(alg));
    token.clear();
    return token;
  }
  token.push_back('.');
  AppendB64Url(token, buf.data(), buf.size());
  return token;
}

DecodedJwt Jwt::tryDecode(std::string_view token, const JwtKey& key, const JwtVerifyOptions& options, JwtError& err) {
  // The verifying key is fixed: the resolver ignores the header "kid" and hands back `key` itself
  // (validity / family are checked inside tryDecodeImpl, exactly as before).
  return tryDecodeImpl(
      token, [](std::string_view, const void* ctx) { return static_cast<const JwtKey*>(ctx); }, &key, options, err);
}

DecodedJwt Jwt::tryDecodeImpl(std::string_view token, KeyResolver resolveKey, const void* ctx,
                              const JwtVerifyOptions& options, JwtError& err) {
  // On failure `out._payloadJson` is left empty, which is exactly what valid() tests, so a
  // partially-populated `out` still reports valid() == false.
  DecodedJwt out;

  // Split the compact serialization into exactly three segments.
  const auto firstDot = token.find('.');
  if (firstDot == std::string_view::npos) {
    err = JwtError::Malformed;
    return out;
  }
  const auto secondDot = token.find('.', firstDot + 1);
  if (secondDot == std::string_view::npos || token.find('.', secondDot + 1) != std::string_view::npos) {
    err = JwtError::Malformed;  // missing signature segment, or a 5-part JWE (unsupported)
    return out;
  }
  const std::string_view headerB64 = token.substr(0, firstDot);
  const std::string_view payloadB64 = token.substr(firstDot + 1, secondDot - firstDot - 1);
  const std::string_view signatureB64 = token.substr(secondDot + 1);
  const std::string_view signingInput = token.substr(0, secondDot);

  // --- header ---
  RawChars32 headerJson;
  JwtJoseHeader headerDoc;
  // Our decoded buffers are not null-terminated, so tell glaze to honour the end pointer strictly
  // (the default assumes a '\0' sentinel and would over-read, e.g. on a trailing bare number).
  if (!DecodeSegment(headerB64, headerJson) || glz::read<glz::opts{.null_terminated = false}>(headerDoc, headerJson)) {
    err = JwtError::Malformed;
    return out;
  }
  if (!headerDoc.crit.str.empty()) {
    // We implement no header-parameter extensions, so any "crit" we cannot satisfy → reject.
    err = JwtError::Malformed;
    return out;
  }
  if (headerDoc.alg.str.empty()) {
    err = JwtError::UnsupportedAlg;
    return out;
  }
  std::string algName;
  if (glz::read<glz::opts{.null_terminated = false}>(algName, headerDoc.alg.str)) {
    err = JwtError::UnsupportedAlg;
    return out;
  }
  JwtAlgorithm alg{};
  if (!FromString(algName, alg)) {
    err = JwtError::UnsupportedAlg;  // unknown algorithm or the unsecured "none"
    return out;
  }
  if (!options.allowedAlgorithms.empty() && !options.allowedAlgorithms.contains(alg)) {
    err = JwtError::AlgNotAllowed;
    return out;
  }
  // Select the verifying key now that the algorithm is known: peek the JOSE header "kid" (absent →
  // empty view) and let the resolver pick. nullptr / an invalid / wrong-family key all → KeyMismatch.
  std::string kidValue;
  const auto kidState = ReadOptionalJsonString(headerDoc.kid, kidValue);
  const std::string_view resolverKid =
      kidState == JsonStringState::String ? std::string_view(kidValue) : std::string_view{};
  const JwtKey* pKey = resolveKey(resolverKid, ctx);
  if (pKey == nullptr || !pKey->valid() || !pKey->matchesFamily(alg)) {
    err = JwtError::KeyMismatch;
    return out;
  }

  // --- signature (verified before any claim is trusted) ---
  RawChars signature;
  if (!DecodeSegment(signatureB64, signature)) {
    err = JwtError::Malformed;
    return out;
  }
  if (!pKey->verify(alg, signingInput, std::string_view(signature.data(), signature.size()))) {
    err = JwtError::InvalidSignature;
    return out;
  }

  // --- claims ---
  RawChars32 payloadJson;
  glz::generic claims;
  if (!DecodeSegment(payloadB64, payloadJson) ||
      glz::read<glz::opts{.null_terminated = false}>(claims,
                                                     std::string_view(payloadJson.data(), payloadJson.size())) ||
      !claims.is_object()) {
    err = JwtError::Malformed;
    return out;
  }

  out._alg = alg;
  std::string typValue;
  const auto typState = ReadOptionalJsonString(headerDoc.typ, typValue);
  if (kidState == JsonStringState::PresentNonString || typState == JsonStringState::PresentNonString ||
      !ReadClaim(claims, "iss", out._claims, DecodedJwt::kIssuer) ||
      !ReadClaim(claims, "sub", out._claims, DecodedJwt::kSubject) ||
      !ReadClaim(claims, "jti", out._claims, DecodedJwt::kJwtId) || !ReadTime(claims, "exp", out._exp) ||
      !ReadTime(claims, "nbf", out._nbf) || !ReadTime(claims, "iat", out._iat)) {
    err = JwtError::Malformed;
    return out;
  }
  if (kidState == JsonStringState::String) {
    out._claims.set(DecodedJwt::kKeyId, kidValue);
  }
  if (typState == JsonStringState::String) {
    out._claims.set(DecodedJwt::kType, typValue);
  }
  if (claims.contains("aud")) {
    const auto& aud = claims.at("aud");
    if (aud.is_string()) {
      if (!AppendAudience(out._aud, aud.get_string())) {
        err = JwtError::Malformed;
        return out;
      }
    } else if (aud.is_array()) {
      for (const auto& el : aud.get_array()) {
        if (!el.is_string() || !AppendAudience(out._aud, el.get_string())) {
          err = JwtError::Malformed;
          return out;
        }
      }
    } else {
      err = JwtError::Malformed;
      return out;
    }
  }

  // --- registered-claim validation ---
  const auto nowTp = options.clock.time_since_epoch().count() == 0 ? std::chrono::system_clock::now() : options.clock;
  const auto nowSecs = std::chrono::duration_cast<std::chrono::seconds>(nowTp.time_since_epoch()).count();
  const auto leeway = options.leeway.count();

  if (options.requireExpiration && out._exp == 0) {
    err = JwtError::MissingExpiration;
    return out;
  }
  if (options.validateExpiration && out._exp != 0 && nowSecs > out._exp + leeway) {
    err = JwtError::Expired;
    return out;
  }
  if (options.validateNotBefore && out._nbf != 0 && nowSecs + leeway < out._nbf) {
    err = JwtError::NotYetValid;
    return out;
  }
  if (!options.issuer.empty() && out._claims[DecodedJwt::kIssuer] != options.issuer) {
    err = JwtError::IssuerMismatch;
    return out;
  }
  if (!options.subject.empty() && out._claims[DecodedJwt::kSubject] != options.subject) {
    err = JwtError::SubjectMismatch;
    return out;
  }
  if (!options.audience.empty() && !out.hasAudience(options.audience)) {
    err = JwtError::AudienceMismatch;
    return out;
  }

  out._headerJson = std::move(headerJson);
  out._payloadJson = std::move(payloadJson);
  err = JwtError::None;
  return out;
}

}  // namespace aeronet
