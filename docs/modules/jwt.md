# JWT

The JWT module signs and verifies JSON Web Tokens ([RFC 7519](https://www.rfc-editor.org/rfc/rfc7519)) in the JWS (signature) profile, and reads the JSON Web Key Sets that identity providers publish. Encrypted tokens (JWE) are out of scope.

The module reuses the OpenSSL crypto already linked for TLS and the Glaze JSON parser, so it adds no dependency. The `AERONET_ENABLE_JWT` CMake option defaults to `ON` whenever both `AERONET_ENABLE_OPENSSL` and `AERONET_ENABLE_GLAZE` are enabled; set it to `OFF` to leave the module out.

| Header | Content |
| --- | --- |
| `<aeronet/jwt.hpp>` | `Jwt::encode()`, `Jwt::tryDecode()`, `JwtVerifyOptions`, `DecodedJwt` |
| `<aeronet/jwt-key.hpp>` | `JwtKey` |
| `<aeronet/jwks.hpp>` | `Jwks` |
| `<aeronet/jwt-algorithm.hpp>`, `<aeronet/jwt-error.hpp>` | `JwtAlgorithm`, `JwtError` |

## Sign and verify

```cpp
#include <aeronet/jwt.hpp>

#include <string>
#include <string_view>

const JwtKey key = JwtKey::Hmac("a-long-random-secret-from-a-secret-store");
const std::string token = Jwt::encode(R"({"sub":"alice","exp":4102444800})", key, JwtAlgorithm::HS256);

JwtVerifyOptions options;
options.allowedAlgorithms = JwtAlgorithmSet{JwtAlgorithm::HS256};

JwtError err = JwtError::None;
if (const DecodedJwt decoded = Jwt::tryDecode(token, key, options, err)) {
  const std::string_view subject = decoded.subject();  // "alice"
} else {
  const std::string_view reason = ToString(err);
}
```

`Jwt::encode()` takes the claims as a JSON object and returns the compact serialization `header.payload.signature`. An optional fourth argument sets the `kid` header. `Jwt::tryDecode()` verifies the signature, then the claims, and returns the decoded token.

## Keys and algorithms

| Family | Algorithms | Key |
| --- | --- | --- |
| HMAC | `HS256`, `HS384`, `HS512` | `JwtKey::Hmac(secret)` |
| RSA PKCS#1 v1.5 | `RS256`, `RS384`, `RS512` | `JwtKey::FromPem(pem)` with an RSA key |
| RSA-PSS | `PS256`, `PS384`, `PS512` | `JwtKey::FromPem(pem)` with an RSA key |
| ECDSA | `ES256`, `ES384`, `ES512` | `JwtKey::FromPem(pem)` with a P-256, P-384, or P-521 key |
| EdDSA | `EdDSA` | `JwtKey::FromPem(pem)` with an Ed25519 key |

A private-key PEM both signs and verifies, a public-key PEM only verifies. `JwtKey::FromJwk(json)` loads a single JSON Web Key ([RFC 7517](https://www.rfc-editor.org/rfc/rfc7517)) of type `oct`, `RSA`, `EC`, or `OKP` (Ed25519). A key that fails to load is invalid (`valid()` is `false`) rather than throwing.

## Claim validation

`JwtVerifyOptions` selects the checks applied after the signature is verified:

| Option | Default | Check |
| --- | --- | --- |
| `allowedAlgorithms` | empty: any algorithm of the key's family | The token's `alg` must be in the set. |
| `validateExpiration`, `validateNotBefore` | `true` | `exp` must be in the future and `nbf` in the past. |
| `requireExpiration` | `false` | Reject a token without `exp`. |
| `leeway` | 0 s | Clock skew tolerated by the `exp` and `nbf` checks. |
| `clock` | now | Reference time of the temporal checks. Set it to test with a fixed time. |
| `issuer`, `subject` | empty: not checked | `iss` and `sub` must be present and equal. |
| `audience` | empty: not checked | `aud`, a string or an array, must contain this value. |

`DecodedJwt` exposes the registered claims through typed accessors: `issuer()`, `subject()`, `audiences()` and `hasAudience()`, `jwtId()`, `expiresAt()`, `notBefore()`, `issuedAt()`, and the header's `algorithm()`, `keyId()`, and `type()`. An absent claim is an empty view or a `0` date. `payloadJson()` returns the verified payload, to parse application claims, for instance with Glaze.

Issuer and audience checks are security decisions: a token signed by a trusted key but meant for another service must be rejected, so set `audience` for every service that accepts third-party tokens.

## Key sets (JWKS)

Identity providers publish their public keys as a JWK Set, `{"keys":[...]}`, typically at a `jwks_uri`. `Jwks` parses it and selects the verification key from the token's `kid` header:

```cpp
#include <aeronet/jwks.hpp>
#include <aeronet/jwt.hpp>

#include <string_view>

const std::string_view jwksJson = R"({"keys":[]})";  // fetched from the provider's jwks_uri
const std::string_view token = "...";

const Jwks keys(jwksJson);  // unsupported keys are skipped; an unparseable document leaves the set empty

JwtVerifyOptions options;
options.issuer = "https://issuer.example.com/";
options.audience = "orders-api";
options.requireExpiration = true;

JwtError err = JwtError::None;
const DecodedJwt decoded = keys.tryDecode(token, options, err);
```

When the token has no `kid` and the set holds exactly one key, that key is used. A `kid` that matches no key fails with `KeyMismatch`. Parsing is transport-agnostic: fetch the document with the [HTTP client](http-client.md) or any other means, cache it, and refresh it when tokens start carrying an unknown `kid`, as providers rotate keys.

## Protect routes with middleware

A request middleware can authenticate every request before it reaches the handlers:

```cpp
#include <aeronet/jwt.hpp>

#include <string_view>

Router router;
router.addRequestMiddleware([](HttpRequestView& request) {
  static const JwtKey key = JwtKey::Hmac("a-long-random-secret-from-a-secret-store");

  JwtVerifyOptions options;
  options.allowedAlgorithms = JwtAlgorithmSet{JwtAlgorithm::HS256};
  options.audience = "orders-api";

  static constexpr std::string_view kBearer = "Bearer ";
  const std::string_view authorization = request.headerValueOrEmpty("authorization");
  JwtError err = JwtError::None;
  if (!authorization.starts_with(kBearer) ||
      !Jwt::tryDecode(authorization.substr(kBearer.size()), key, options, err)) {
    return MiddlewareResult::ShortCircuit(
        HttpResponse(http::StatusCodeUnauthorized).headerAddLine("www-authenticate", "Bearer"));
  }
  return MiddlewareResult::Continue();
});
```

Verification runs on the event-loop thread. RSA and ECDSA signatures cost much more to verify than HMAC ones: include them in the CPU budget of a server that authenticates every request.

## Security posture

- The unsecured `alg: none` (RFC 7518 §3.6) is always rejected, so a token with a stripped signature never verifies.
- A key is only used with algorithms of its own family: an HMAC key cannot verify an `RS256` token (`KeyMismatch`). This rules out the classic confusion where an RSA public key is used as an HMAC secret. `allowedAlgorithms` narrows the accepted set further.
- The signature is verified before any claim is parsed or trusted.
- A `crit` header that names an extension aeronet does not implement is rejected (RFC 7515 §4.1.11).
- HMAC signatures are compared in constant time (`CRYPTO_memcmp`).

## Error model

The module never throws. `JwtKey` factories return an invalid key, `Jwt::encode()` returns an empty string, and `tryDecode()` returns an invalid `DecodedJwt` and sets a `JwtError`. Every failure is also logged. `ToString(JwtError)` describes a reason.

| `JwtError` | Cause |
| --- | --- |
| `Malformed` | Not three base64url segments, or invalid header or claims JSON. |
| `UnsupportedAlg` | Missing, unknown, or `none` algorithm. |
| `AlgNotAllowed` | Algorithm outside `allowedAlgorithms`. |
| `KeyMismatch` | Key family incompatible with the algorithm, or no key for the `kid`. |
| `InvalidSignature` | Signature does not verify. |
| `Expired`, `NotYetValid`, `MissingExpiration` | Temporal checks. |
| `IssuerMismatch`, `AudienceMismatch`, `SubjectMismatch` | Claim checks. |

## Limitations

- JWE (encrypted tokens) is not supported.
- `Jwks` parses a document but does not fetch or cache it.

## Tests

[aeronet/jwt/test/](../../aeronet/jwt/test/): [jwt-roundtrip_test.cpp](../../aeronet/jwt/test/jwt-roundtrip_test.cpp) covers every algorithm, [jwt-claims_test.cpp](../../aeronet/jwt/test/jwt-claims_test.cpp) the claim checks, [jwt-decode-errors_test.cpp](../../aeronet/jwt/test/jwt-decode-errors_test.cpp) the rejection paths (`alg: none`, `crit`, algorithm and key mismatch), and [jwt-jwk_test.cpp](../../aeronet/jwt/test/jwt-jwk_test.cpp) JWK and JWKS parsing.
