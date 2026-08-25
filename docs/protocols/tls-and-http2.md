# TLS and HTTP/2

TLS and HTTP/2 are opt-in modules. Enable them at CMake configure time so applications that only need HTTP/1.1 do not pay for their dependencies or binary surface.

## TLS

Set `AERONET_ENABLE_OPENSSL=ON` to build the OpenSSL 3 based server transport. Each server owns its `SSL_CTX` and per-server policy. Handshakes use the existing non-blocking event loop, so certificate processing, callbacks, and application-provided revocation checks run on the handshake path and must not block.

### Minimal HTTPS listener

The file form expects a PEM certificate or full chain and its matching PEM private key:

```cpp
#include <aeronet/aeronet-server.hpp>

#include <chrono>

using namespace std::chrono_literals;

aeronet::HttpServerConfig config;
config.withPort(443)
    .withTlsCertKey("/run/tls/fullchain.pem", "/run/tls/private.key")
    .withTlsAlpnProtocols({"h2", "http/1.1"})
    .withTlsHandshakeTimeout(10s);
config.tls.withTlsMinVersion("TLS1.2");

aeronet::SingleHttpServer server(std::move(config));
server.run();
```

`withTlsCertKeyMemory(certPem, keyPem)` is available when a secret manager or another component already holds the PEM data. Avoid unnecessary `std::string` copies of private keys and keep caller-owned buffers alive only as long as needed.

When no minimum is configured, aeronet still enforces TLS 1.2 as the effective minimum. TLS 1.3 remains available. The context also prefers the server's cipher order, disables renegotiation where the OpenSSL build exposes that control, and disables TLS compression by default. Explicit protocol bounds accept `TLS1.2` and `TLS1.3`.

### Certificates and SNI

The main certificate is the fallback when the client sends no SNI name or no mapping matches. Add exact or single label wildcard routes through `TLSConfig`:

```cpp
aeronet::HttpServerConfig config;
config.tls
    .withTlsSniCertificateFiles(
        "api.example.com", "/run/tls/api.crt", "/run/tls/api.key")
    .withTlsSniCertificateFiles(
        "*.edge.example.com", "/run/tls/edge.crt", "/run/tls/edge.key");
```

Names are normalized to lowercase. A wildcard such as `*.edge.example.com` matches exactly one non-empty label, such as `api.edge.example.com`; it matches neither the bare `edge.example.com` nor `deep.api.edge.example.com`. Certificate files and in-memory PEM routes may be mixed.

### Versions, ciphers, and ALPN

`withTlsCipherPolicy()` applies one of the predefined `Modern`, `Compatibility`, or `Legacy` policies to TLS 1.3 suites and TLS 1.2-and-earlier cipher lists. `Default` leaves OpenSSL's cipher selection in place. A raw `withTlsCipherList()` setting affects the OpenSSL pre-TLS-1.3 list; prefer a named policy unless exact compatibility requirements justify maintaining a custom expression.

ALPN follows server preference order. For a TLS endpoint serving both HTTP versions, use:

```cpp
aeronet::HttpServerConfig config;
config.withTlsAlpnProtocols({"h2", "http/1.1"});
```

With `withTlsAlpnMustMatch(true)`, the handshake fails if the client offers no configured protocol. Without strict mode, the connection may continue without ALPN and use HTTP/1.1. The negotiated protocol, version, and cipher are available on `HttpRequestView` and in TLS statistics.

### Mutual TLS

Client-certificate handling has two modes:

- `withTlsRequestClientCert()` asks for a certificate but allows the client to omit it. A presented certificate must
  still verify.
- `withTlsRequireClientCert()` requires a valid certificate and implies request mode.

Add one or more PEM trust anchors or explicitly trusted leaves with `withTlsTrustedClientCert()`:

```cpp
aeronet::HttpServerConfig config;
std::string_view clientCaPem = "...";
config.withTlsTrustedClientCert(clientCaPem)
      .withTlsRequireClientCert();
```

Strict mTLS validation rejects a configuration with no trust material. The configured trust store, CRLs, and revocation callback apply to inbound client certificates. They do not configure revocation checking for outbound connections made by `HttpClient`.

### Revocation and certificate status

OCSP stapling and client-certificate revocation solve different problems:

| Control | Certificate direction | Handshake behavior |
| --- | --- | --- |
| Cached OCSP staple | Server certificate presented to clients | Returns operator-provided DER bytes when the client asks for status |
| CRL | Client certificate presented to the server | OpenSSL rejects a revoked leaf or chain during verification |
| Application callback | Client certificate presented to the server | Application may reject an otherwise valid certificate at each chain depth |

#### Passive cached OCSP stapling

Configure a pre-fetched DER `OCSPResponse` for the default certificate:

```cpp
aeronet::HttpServerConfig config;
config.withTlsOcspStapleFile("/run/tls/default.ocsp.der");
```

For SNI, add the certificate first and then associate its response with the same hostname pattern:

```cpp
aeronet::HttpServerConfig config;
config.tls
    .withTlsSniCertificateFiles(
        "api.example.com", "/run/tls/api.crt", "/run/tls/api.key")
    .withTlsSniOcspStapleFile(
        "api.example.com", "/run/tls/api.ocsp.der");
```

At context creation, aeronet reads the file once, caps it at 1 MiB, parses exactly one DER OCSP response, and requires its top-level status to be `successful`. The cached bytes are copied into each handshake response without disk access or responder traffic. A missing, empty, oversized, malformed, trailing-data, or non-successful response fails context creation. During a failed hot reload, the old context remains active.

This is intentionally passive. aeronet does not:

- discover or contact an OCSP responder;
- prove that the supplied response belongs to the configured certificate;
- verify its signature or responder authorization; or
- enforce `producedAt`, `thisUpdate`, or `nextUpdate` freshness.

The provisioning job must fetch and cryptographically verify each response against the leaf, issuer, and authorized responder chain before publishing it. It must also refresh the file before `nextUpdate`. Replace the file atomically, then ask the server to rebuild the TLS context:

```cpp
aeronet::SingleHttpServer server;
server.postConfigUpdate([](aeronet::HttpServerConfig& current) {
  current.tls.withTlsOcspStapleFile("/run/tls/default.ocsp.der");
});
```

The same update mechanism refreshes per-SNI paths. New connections use the new context; established TLS connections continue with the context under which they were created.

Inspect the provisioned response separately, then confirm the live endpoint returns it:

```bash
openssl ocsp -respin /run/tls/default.ocsp.der -text -noverify
openssl s_client -connect localhost:443 -servername example.com -status </dev/null
```

`-noverify` in the first command is suitable only for decoding and inspection. It is not a replacement for the provisioning job's signature, issuer, certificate-ID, and freshness checks.

#### CRL verification for client certificates

CRL input may be PEM or DER. It requires request or require mTLS mode:

```cpp
aeronet::HttpServerConfig config;
std::string_view clientCaPem = "...";
config.withTlsTrustedClientCert(clientCaPem)
      .withTlsRequireClientCert()
      .withTlsCrlFile("/run/tls/client-ca.crl", false);
```

The default `false` enables `X509_V_FLAG_CRL_CHECK`, which checks the client leaf. Passing `true` also enables `X509_V_FLAG_CRL_CHECK_ALL`; the verification store then needs suitable CRLs for every applicable issuer in the verified chain. A missing or invalid CRL fails context creation. CRL freshness and replacement scheduling remain operator responsibilities.

#### Application revocation hook

The optional callback is useful for a bounded in-memory denylist, a previously refreshed OCSP/CRL index, or an enterprise policy engine whose decision is already local. Its signature is:

```cpp
using TlsRevocationCallback = aeronet::TlsRevocationStatus (*)(
    aeronet::TlsPeerCertificateView certificate,
    void* userContext) noexcept;
```

OpenSSL chain and CRL verification runs first. The hook is invoked only for certificates that pass that step, once per verified chain depth. Return `Good` or `NoOpinion` to continue and `Revoked` to fail with a revoked-certificate verification error. An invalid enum value fails closed. The hook cannot override an invalid signature, expired certificate, untrusted chain, or CRL failure.

```cpp
#include <openssl/x509.h>

aeronet::HttpServerConfig config;
struct RevocationCache {
  bool contains(const X509* certificate) const noexcept {
    // Perform a bounded lookup in an immutable local cache.
    return false;
  }
} cache;

auto checkClientCertificate = +[](aeronet::TlsPeerCertificateView view,
                                  void* context) noexcept {
  const auto* currentCache = static_cast<const RevocationCache*>(context);
  const auto* certificate = static_cast<const X509*>(view.nativeCertificate);
  return currentCache->contains(certificate)
             ? aeronet::TlsRevocationStatus::Revoked
             : aeronet::TlsRevocationStatus::Good;
};

config.withTlsRequestClientCert()
      .withTlsRevocationCallback(checkClientCertificate, &cache);
```

`nativeCertificate` is an OpenSSL `X509*` exposed opaquely so the public configuration header does not require OpenSSL headers. It is valid only for the callback invocation. Do not retain it. The callback is declared `noexcept` and executes synchronously on the event-loop handshake path, so it must not allocate unpredictably, perform network I/O, wait on a contended service, or throw. Build a new immutable cache off-thread and publish it safely instead.

CRL and callback checks may be combined. Both must accept the certificate for verification to continue.

### Session tickets

Tickets are disabled by default. Enable automatic random-key rotation with:

```cpp
aeronet::HttpServerConfig config;
config.tls.withTlsSessionTickets(true)
          .withTlsSessionTicketLifetime(std::chrono::hours{24})
          .withTlsSessionTicketMaxKeys(2);
```

The store retains the configured key window so recently issued tickets can be decrypted after rotation. A `MultiHttpServer` shares the store across its workers. Supplying one or more 48-byte static keys enables tickets and disables automatic generation; provision and rotate shared static keys carefully. Session resumption is supported, but aeronet does not enable TLS 1.3 early data.

### Debug-only traffic key logging

For packet-level debugging, `withTlsKeyLogFile()` appends OpenSSL key-log callback output in the NSS `SSLKEYLOGFILE` format:

```cpp
#ifndef NDEBUG
aeronet::HttpServerConfig config;
config.withTlsKeyLogFile("/tmp/aeronet-debug.keys");
#endif
```

Release builds, identified by `NDEBUG`, reject a non-empty key-log path during validation and context construction. On POSIX, aeronet creates a new file with mode `0600`, append mode, and close-on-exec. It does not weaken or repair permissions on an existing file, so inspect or pre-create that file securely. Writes from the context are serialized.

!!! danger
    A key log allows anyone with the corresponding packet capture to decrypt TLS application data. Use it only in an
    isolated debug environment. Never enable it in production, include it in a container image, upload it with normal
    logs, or retain it after diagnosis.

Wireshark can consume the file through Preferences, Protocols, TLS, `(Pre)-Master-Secret log filename`.

### Hot reload and failure behavior

Any `TLSConfig` change submitted through `SingleHttpServer::postConfigUpdate()` or `MultiHttpServer::postConfigUpdate()` rebuilds the context. This covers certificates, private keys, trust anchors, SNI mappings, OCSP responses, CRLs, key-log settings, ciphers, protocol bounds, and ticket settings.

- The update is applied on the server loop.
- New connections receive the rebuilt context.
- Existing connections retain their established TLS state.
- Validation, credential, OCSP, CRL, or OpenSSL setup failure keeps the previous config and context active.

Keep input files local and small because reading and parsing them is part of the update. Publish files using an atomic rename so the server never observes a partially written credential or status response.

### Hardening and secret lifecycle

The hardening audit distinguishes defaults, aeronet-owned memory, and memory outside the library's control:

| Area | Confirmed behavior | Boundary |
| --- | --- | --- |
| Protocol | Effective minimum TLS 1.2; TLS 1.3 supported | Applications may set an explicit TLS 1.2/1.3 maximum |
| Context options | Server cipher preference; no renegotiation where supported; compression disabled by default | OpenSSL and platform capabilities still determine negotiated primitives |
| In-memory private keys | Main and per-SNI PEM key regions are overwritten before replacement and on `TLSConfig` destruction | Caller-owned PEM buffers and allocator/OS copies are not owned by aeronet |
| Static ticket keys | Temporary by-value input and stored keys are overwritten on growth, clear, assignment, and destruction | Original caller key objects must be scrubbed by the caller when no longer needed |
| Runtime ticket keys | One contiguous 48-byte object is cleansed with `OPENSSL_cleanse` on rotation/removal/destruction | Serialized tickets held by clients remain valid within the active key window |
| Ticket lookup | Key names use `CRYPTO_memcmp` | This protects the local comparison, not surrounding application logic |
| OpenSSL session secrets | Owned by `SSL`/`SSL_CTX` and released through OpenSSL RAII lifecycles | OpenSSL controls their internal allocation and cleansing guarantees |
| Key log | Disabled unless explicitly configured, and rejected in release builds | Enabling it deliberately exports traffic secrets and overrides confidentiality expectations |

Zeroization is therefore best-effort within explicit ownership, not a promise that a process or machine contains no historical secret bytes. Copies may remain in caller strings, allocator arenas, filesystem cache, swap, crash dumps, debuggers, OpenSSL internals, kernel memory, backups, and key-log files. For high-assurance deployments, combine the library controls with locked-down secret injection, disabled core dumps, encrypted or disabled swap, restricted debug access, short secret lifetimes, and process isolation.

### Handshake resilience, observability, and kTLS

Use a non-zero handshake timeout plus concurrency and rate limits for public endpoints:

```cpp
aeronet::HttpServerConfig config;
config.withTlsHandshakeTimeout(std::chrono::seconds{10});
config.tls.withTlsHandshakeConcurrencyLimit(1024)
          .withTlsHandshakeRateLimit(500, 1000);
```

`withTlsHandshakeLogging()` adds a negotiated summary. `ServerStats` exposes successful/full/resumed handshakes, failure-reason buckets, strict ALPN mismatches, client-certificate presence, handshake duration, and negotiated ALPN, version, and cipher distributions. Handshake callbacks are also available when structured event integration is more appropriate than logs.

Linux kTLS can offload eligible TLS sends. `Opportunistic` is the default and falls back to user-space TLS. `Enabled` also warns when offload is unavailable, `Required` closes a connection that cannot enable it, and `Disabled` never tries. Treat kTLS as a transport optimization, not a change to certificate or revocation policy.

### Deployment checklist

- Build and test with the same OpenSSL major version used in production.
- Serve the full certificate chain and verify the private key matches.
- Keep TLS 1.2 or newer and use an intentional cipher policy.
- Configure ALPN explicitly when HTTP/2 is enabled.
- Set handshake timeout and admission limits for exposed listeners.
- Choose request versus require mTLS deliberately and provide the correct trust anchors.
- If stapling OCSP, verify certificate binding, signature, authorization, and freshness before publishing each DER
  response; alert before `nextUpdate`.
- If checking client revocation, refresh CRLs or callback data before expiry and test fail-closed behavior.
- Keep key logging out of release builds and production images.
- Monitor handshake failures, version/cipher distribution, resumption, and kTLS fallback counters.
- Exercise `postConfigUpdate()` failure and rollback behavior before relying on automated rotation.

For the complete setting list, see [Server configuration](../reference/server-configuration.md#tls-configuration).
The [production configuration guide](../guides/production-configuration.md) covers listener-level deployment patterns.

## HTTP/2

Set `AERONET_ENABLE_HTTP2=ON`, then enable the feature in `Http2Config`. HTTP/2 can be used over TLS through ALPN or in clear text through h2c when that is appropriate for your environment.

The same request-handler model is used for HTTP/1.1 and HTTP/2, so handlers can inspect protocol-specific properties only where necessary. See [HTTP/2](../FEATURES.md#http2-rfc-9113) for configuration, h2c, stream behavior, and test commands. A complete [HTTP/2 example](../../examples/http2.cpp) demonstrates both TLS and h2c modes.

```cpp
aeronet::HttpServerConfig config;
aeronet::Http2Config http2;
http2.withMaxConcurrentStreams(128)
    .withPingInterval(std::chrono::seconds{30})
    .withEnableH2c(false)
    .withEnableH2cUpgrade(false);

config.withTlsAlpnProtocols({"h2", "http/1.1"}).withHttp2(http2);
```

!!! warning
    HTTP/2 flow control, concurrent stream limits, and TLS certificate management are production settings. Set them
    deliberately for your workload instead of relying on benchmark-oriented values.
