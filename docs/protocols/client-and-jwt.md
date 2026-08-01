# HTTP client and JWT

aeronet includes optional client and JWT modules that reuse the library's transport, TLS, and crypto foundations.

## HTTP client

The HTTP client supports synchronous HTTP/1.1 requests, connection reuse, redirects, automatic compression handling, and optional HTTPS. Enable the client module in your build, then start with the [client example](https://github.com/sjanel/aeronet/blob/main/examples/client-minimal.cpp).

Its public behavior and current boundaries are summarized in the [feature reference](../FEATURES.md#future-expansions). Consult the source tests when adding a path that depends on redirect, connection-pool, or decompression edge cases.

## JWT and JWKS

The opt-in JWT module implements the JWS profile of RFC 7519. It supports signing and verification with HS, RS, ES, PS, and EdDSA algorithms, plus JWK/JWKS lookup by `kid`. It rejects `alg: none` and performs claim validation through an explicit API.

Read [JWT](../FEATURES.md#jwt-rfc-7519--jws-profile) for algorithm availability, claim validation, key discovery, and security guidance. Treat key rotation, issuer/audience validation, and clock handling as application security decisions rather than defaults to skip.
