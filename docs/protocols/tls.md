# TLS

aeronet terminates TLS with OpenSSL 3. The `AERONET_ENABLE_OPENSSL` CMake option builds the TLS transport; it defaults to `ON` when aeronet is the top-level project, and must be enabled explicitly when aeronet is consumed as a dependency. When it is `OFF`, the core stays free of OpenSSL headers and libraries.

Each server owns its `SSL_CTX` and its TLS policy: there is no process-global mutable TLS state, so two servers in one process can use different certificates, versions, and client-certificate rules. Handshakes run on the non-blocking event loop, so certificate processing, callbacks, and application-provided revocation checks run on the handshake path and must not block.

| Area | What aeronet provides |
| --- | --- |
| Certificates | PEM files or in-memory PEM, SNI-based certificate selection with wildcards, hot reload without restart. |
| Protocol policy | TLS 1.2 minimum by default, TLS 1.3, named cipher policies, ALPN with optional strict matching. |
| Client certificates | Optional or required mTLS, trust anchors, CRLs, application revocation hook. |
| Server certificate status | Passive OCSP stapling, globally or per SNI certificate. |
| Performance | Session tickets with key rotation, Linux kernel TLS (kTLS) offload, record-aware reads and writes. |
| Protection | Handshake timeout, concurrency and rate limits. |
| Observability | Handshake logging and callback, per-server statistics (handshakes, failures, versions, ciphers, ALPN, kTLS). |

## Minimal HTTPS listener

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

## Certificates and SNI

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

## Versions, ciphers, and ALPN

`withTlsCipherPolicy()` applies one of the predefined `Modern`, `Compatibility`, or `Legacy` policies to TLS 1.3 suites and TLS 1.2-and-earlier cipher lists. `Default` leaves OpenSSL's cipher selection in place. A raw `withTlsCipherList()` setting affects the OpenSSL pre-TLS-1.3 list; prefer a named policy unless exact compatibility requirements justify maintaining a custom expression.

ALPN follows server preference order. For a TLS endpoint serving both HTTP versions, use:

```cpp
aeronet::HttpServerConfig config;
config.withTlsAlpnProtocols({"h2", "http/1.1"});
```

The server selects the first protocol of its own list that the client also offered. With `withTlsAlpnMustMatch(true)`, the handshake fails when the client offers none of them, and the `tlsAlpnStrictMismatches` statistic is incremented. Without strict mode, the connection continues without ALPN and uses HTTP/1.1.

Handlers can read the negotiated parameters of their connection with `HttpRequestView::alpnProtocol()`, `tlsVersion()`, and `tlsCipher()`, which return empty views on a plaintext connection. The same values feed the [TLS statistics](#observability).

## Mutual TLS

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

## Revocation and certificate status

OCSP stapling and client-certificate revocation solve different problems:

| Control | Certificate direction | Handshake behavior |
| --- | --- | --- |
| Cached OCSP staple | Server certificate presented to clients | Returns operator-provided DER bytes when the client asks for status |
| CRL | Client certificate presented to the server | OpenSSL rejects a revoked leaf or chain during verification |
| Application callback | Client certificate presented to the server | Application may reject an otherwise valid certificate at each chain depth |

### Passive cached OCSP stapling

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

The provisioning job must fetch and cryptographically verify each response against the leaf, issuer, and authorized responder chain before publishing it. It must also refresh the response before `nextUpdate`. Publish each new response atomically at the configured path (write a temporary file in the same directory, then rename it over the path), then post a config update: the server reloads the files that changed on disk, even an empty update triggers it (see [Hot reload](#hot-reload-and-failure-behavior)):

```cpp
aeronet::SingleHttpServer server;
// After renaming the new response over /run/tls/default.ocsp.der:
server.postConfigUpdate([](aeronet::HttpServerConfig&) {});
```

Posting a new path with `withTlsOcspStapleFile()` works too. The same update mechanism refreshes per-SNI responses. New connections use the new context; established TLS connections continue with the context under which they were created.

Inspect the provisioned response separately, then confirm the live endpoint returns it:

```bash
openssl ocsp -respin /run/tls/default.ocsp.der -text -noverify
openssl s_client -connect localhost:443 -servername example.com -status </dev/null
```

`-noverify` in the first command is suitable only for decoding and inspection. It is not a replacement for the provisioning job's signature, issuer, certificate-ID, and freshness checks.

### CRL verification for client certificates

CRL input may be PEM or DER. It requires request or require mTLS mode:

```cpp
aeronet::HttpServerConfig config;
std::string_view clientCaPem = "...";
config.withTlsTrustedClientCert(clientCaPem)
      .withTlsRequireClientCert()
      .withTlsCrlFile("/run/tls/client-ca.crl", false);
```

The default `false` enables `X509_V_FLAG_CRL_CHECK`, which checks the client leaf. Passing `true` also enables `X509_V_FLAG_CRL_CHECK_ALL`; the verification store then needs suitable CRLs for every applicable issuer in the verified chain. A missing or invalid CRL fails context creation. CRL freshness and replacement scheduling remain operator responsibilities: a CRL replaced at the same path is reloaded by the next config update (see [Hot reload](#hot-reload-and-failure-behavior)).

### Application revocation hook

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

## Session tickets

Tickets are disabled by default. Enable automatic random-key rotation with:

```cpp
aeronet::HttpServerConfig config;
config.tls.withTlsSessionTickets(true)
          .withTlsSessionTicketLifetime(std::chrono::hours{24})
          .withTlsSessionTicketMaxKeys(2);
```

A session ticket is the encrypted state of a TLS session, kept by the client. Presenting it on a new connection resumes the session with an abbreviated handshake, without a server-side session cache. Tickets are encrypted with 48-byte keys: a 16-byte key name, a 16-byte AES key, and a 16-byte HMAC key.

With automatic rotation, aeronet generates random keys (`RAND_bytes()`) and replaces the current key every `sessionTicketLifetime`. It keeps up to `sessionTicketMaxKeys` keys, so tickets issued before a rotation can still be decrypted, and drops older keys. A `MultiHttpServer` shares one key store between its workers, so a ticket issued by one worker resumes on any other. Session resumption is supported, but aeronet does not enable TLS 1.3 early data.

| Method | Default | Purpose |
| --- | --- | --- |
| `withTlsSessionTickets(bool)` | disabled | Enable tickets with automatic key rotation. |
| `withTlsSessionTicketLifetime(duration)` | 24 hours | Rotation interval of the automatic keys. |
| `withTlsSessionTicketMaxKeys(n)` | 2 | Keys kept for decryption during rotation. |
| `withTlsSessionTicketKey(key)` | none | Append a static key. Enables tickets and disables automatic generation. |
| `clearTlsSessionTicketKeys()` | | Remove (and scrub) the static keys. |

Static keys let several processes or hosts resume each other's sessions, or keep tickets valid across restarts. They must come from a secret store, never from source code:

```cpp
TLSConfig::SessionTicketKey key{};  // 16-byte name + 16-byte AES key + 16-byte HMAC key
// ... fill `key` from a secret manager ...

HttpServerConfig config;
config.withTlsCertKey("/run/tls/fullchain.pem", "/run/tls/private.key");
config.tls.withTlsSessionTicketKey(key);
```

The first static key encrypts new tickets, and every key decrypts. Keys beyond `sessionTicketMaxKeys` are ignored, with a warning. With static keys, rotation is the operator's job, through [hot reload](#hot-reload-and-failure-behavior): install the new key first and the previous one second, then drop the previous one once the tickets it encrypted have expired. Ticket keys protect the resumed sessions: store them like private keys, and rotate them often.

Verify resumption with `openssl s_client`: the second connection should print `Reused`.

```bash
openssl s_client -connect localhost:8443 -sess_out /tmp/session.pem </dev/null
openssl s_client -connect localhost:8443 -sess_in /tmp/session.pem </dev/null | grep -E 'Reused|New'
```

The `tlsHandshakesResumed` and `tlsHandshakesFull` statistics show the resumption ratio in production. The [session tickets example](../../examples/tls-session-tickets.cpp) runs a complete server.

## Debug-only traffic key logging

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

## Hot reload and failure behavior

A `TLSConfig` change submitted through `SingleHttpServer::postConfigUpdate()` or `MultiHttpServer::postConfigUpdate()` rebuilds the TLS context. This covers certificates, private keys, trust anchors, SNI mappings, OCSP responses, CRLs, key-log settings, ciphers, protocol bounds, and ticket settings.

- The update is applied on the server loop.
- New connections receive the rebuilt context.
- Existing connections retain their established TLS state.

Files rotated at the same path are reloaded too. Each applied config update, whatever it changes, checks the files the current context was built from (certificate, private key, OCSP response, CRL, and their SNI counterparts) with one `stat()` per file, and rebuilds the context when one of them was rewritten, replaced, or removed since. The check follows symbolic links and compares the device, inode, size, and modification and status change times (size and modification time on Windows). It therefore detects a file rewritten in place, a file replaced by an atomic rename, and a Kubernetes secret volume update, which swaps the `..data` link of the volume. An empty update is the way to reload rotated files, for instance from a certificate renewal hook or a timer:

```cpp
SingleHttpServer server;

// Reload the certificate, key, OCSP, and CRL files rotated at their configured paths.
server.postConfigUpdate([](HttpServerConfig&) {});

// Swap the certificate and key for new connections.
server.postConfigUpdate([](HttpServerConfig& cfg) {
  cfg.withTlsCertKey("/run/tls/v42/fullchain.pem", "/run/tls/v42/private.key");
});

// Replace the trust anchors of client certificates.
std::string_view newClientCaPem = "...";
server.postConfigUpdate([newClientCaPem](HttpServerConfig& cfg) {
  cfg.tls.withoutTlsTrustedClientCert().withTlsTrustedClientCert(newClientCaPem);
});
```

Files are not watched: nothing is reloaded until an update is applied, and no file is read on the handshake path. In-memory PEM inputs are compared by content with the rest of the `TLSConfig`. A rebuilt context keeps the session ticket keys of the previous one, so that the tickets issued before still resume, unless the update changes the session ticket settings (`sessionTickets` or the static keys).

A failed update leaves the server running with its previous configuration:

- Each posted update is applied atomically. If its updater throws, if the resulting configuration fails validation, or if the new TLS context cannot be built (unreadable or invalid certificate, key not matching the certificate, invalid OCSP response or CRL), the error is logged with the faulty file and the reason, and the whole update is discarded: the previous configuration and TLS context stay active. Updates posted together are independent, a rejected one does not prevent the others from applying.
- If the reload of files changed on disk fails, the previous TLS context stays active, and the reload is attempted again at each later update, until the files are fixed.
- Each `MultiHttpServer` worker applies the update to its own configuration. An invalid input is rejected by every worker alike, so all of them keep serving the same configuration. Workers could only end up different if an input changed while they were applying the update (a file rewritten during the reload, an updater that does not always do the same thing): posting the update again makes them converge.

Write rotated files atomically, so that the server never reads a partially written file: write a temporary file in the same directory, then rename it over the configured path. Replace the certificate and its key before posting the update, or publish them together behind a directory link as Kubernetes does: a reload reading the new certificate with the old key is rejected until the next update. Validate new credentials and status files before publishing them: check that the key matches the certificate, that the chain is complete, and that OCSP and CRL inputs parse. Keep input files local and small, because reading and parsing them is part of the update.

## Hardening and secret lifecycle

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

## Handshake protection

A TLS handshake costs the server far more CPU than the client. Bound handshakes on public listeners with a timeout, a concurrency limit, and a rate limit:

```cpp
HttpServerConfig config;
config.withTlsHandshakeTimeout(std::chrono::seconds{10});
config.tls.withTlsHandshakeConcurrencyLimit(1024)  // handshakes in progress at once
    .withTlsHandshakeRateLimit(500, 1000);         // new handshakes per second, burst
```

A handshake that exceeds the timeout is closed. A connection beyond the concurrency or rate limit is closed right after it is accepted, before any OpenSSL object is allocated, and counted in `tlsHandshakesRejectedConcurrency` or `tlsHandshakesRejectedRateLimit`. When the server closes an established TLS connection, it first sends a best-effort, non-blocking `close_notify`.

## Observability

`withTlsHandshakeLogging()` logs a summary of each handshake: version, cipher, ALPN protocol, and peer certificate subject.

For structured integration, install a handshake callback. It receives one event per handshake that succeeded, failed, or was rejected by the admission limits:

```cpp
#include <aeronet/log.hpp>

SingleHttpServer server;
server.setTlsHandshakeCallback([](const TlsHandshakeEvent& event) {
  if (event.result != TlsHandshakeEvent::Result::Succeeded) {
    log::warn("TLS handshake {} on fd {}: {}", event.result == TlsHandshakeEvent::Result::Failed ? "failed" : "rejected",
              event.fd, event.reason);
  }
});
```

`TlsHandshakeEvent` also carries `resumed`, `clientCertPresent`, `durationNs`, and, for successful handshakes, `selectedAlpn`, `negotiatedVersion`, `negotiatedCipher`, and `peerSubject`. Its string views are valid during the callback only. The callback runs on the event loop: keep it short.

`server.stats()` returns per-server counters, also serializable with `ServerStats::json_str()`:

| `ServerStats` field | Content |
| --- | --- |
| `tlsHandshakesSucceeded`, `tlsHandshakesFull`, `tlsHandshakesResumed` | Successful handshakes, split by resumption. |
| `tlsHandshakesFailed`, `tlsHandshakesRejectedConcurrency`, `tlsHandshakesRejectedRateLimit` | Failed and rejected handshakes. |
| `tlsHandshakeFailureReasons` | Best-effort count of failures and rejections per reason. |
| `tlsClientCertPresent`, `tlsAlpnStrictMismatches` | Client certificates received, strict ALPN failures. |
| `tlsAlpnDistribution`, `tlsVersionCounts`, `tlsCipherCounts` | Negotiated protocols, versions, and ciphers. |
| `tlsHandshakeDurationCount`, `tlsHandshakeDurationTotalNs`, `tlsHandshakeDurationMaxNs` | Handshake duration aggregates. |
| `ktlsSendEnabledConnections`, `ktlsSendEnableFallbacks`, `ktlsSendForcedShutdowns`, `ktlsSendBytes` | Kernel TLS usage, see below. |

```cpp
#include <aeronet/log.hpp>

SingleHttpServer server;
const ServerStats stats = server.stats();
for (const auto& [reason, count] : stats.tlsHandshakeFailureReasons) {
  log::info("tls_handshake_failure reason={} count={}", reason, count);
}
```

The [observability guide](../operations/observability.md) covers logs, OpenTelemetry, and DogStatsD export.

## Kernel TLS (kTLS)

On Linux, kTLS moves the encryption of sent data into the kernel. The server can then send files with `sendfile()` without copying them to user space, and write a response made of several buffers with a single `sendmsg()`. aeronet enables kTLS send per connection after the handshake, when the kernel (the `tls` module), the OpenSSL build, and the negotiated cipher suite support it.

| `TLSConfig::KtlsMode` | Behavior |
| --- | --- |
| `Opportunistic` (default) | Try once per connection; silently fall back to user-space TLS when unavailable. |
| `Enabled` | Same, and log a warning on fallback. |
| `Required` | Close the connection when kTLS cannot be enabled. |
| `Disabled` | Never try. |

```cpp
HttpServerConfig config;
config.withPort(8443)
    .withTlsCertKey("/run/tls/fullchain.pem", "/run/tls/private.key")
    .withTlsKtlsMode(TLSConfig::KtlsMode::Enabled);
```

`ktlsSendEnabledConnections` and `ktlsSendEnableFallbacks` tell whether offload works in a given environment, `ktlsSendBytes` counts the bytes sent through kernel TLS, and `ktlsSendForcedShutdowns` the connections closed by `Required`. Use `Required` only after checking the kernel, OpenSSL, and cipher compatibility of the target environment. kTLS is a transport optimization: it changes neither certificate nor revocation policy. The [kTLS example](../../examples/tls-ktls.cpp) prints these counters.

## TLS record I/O

The server keeps the number of TLS records and system calls per request close to what plaintext connections need:

- **Reads**: server contexts read ahead, so OpenSSL reads as much ciphertext as the socket holds in one system call, instead of two per record (the 5-byte header, then the body). A short read proves the socket is drained, as with a plaintext socket, so the event loop does not issue an extra read returning `EAGAIN`. A peer `close_notify` or an error that follows data is reported after that data is served.
- **Writes with user-space TLS**: OpenSSL makes at least one record per `SSL_write`. Rather than encrypting each buffer of a gather write separately, which would make the 9-byte header of each HTTP/2 frame a record and a system call of its own, aeronet encrypts large buffers in place and coalesces small ones with the following buffers into full 16 KiB records.
- **Writes with kTLS**: data is written in clear with `send()` / `sendmsg()`, and the kernel builds full records across buffers, so a gather write is a single system call.

## HTTP to HTTPS redirect

A plaintext listener can answer every request with a redirect to the equivalent `https://` URL, instead of routing it. Pair it with the TLS listener that serves the application:

```cpp
HttpServerConfig redirectConfig;
redirectConfig.withPort(80).withHttpsRedirect(443, http::StatusCodePermanentRedirect);
SingleHttpServer redirector(redirectConfig);

HttpServerConfig tlsConfig;
tlsConfig.withPort(443).withTlsCertKey("/run/tls/fullchain.pem", "/run/tls/private.key");
SingleHttpServer app(tlsConfig);
```

| Setting | Default | Notes |
| --- | --- | --- |
| `httpsRedirect.targetPort` | `0` (disabled) | Any non-zero port enables the redirect. Port 443 is omitted from the URL, other ports are appended (`https://host:8443/path`). |
| `httpsRedirect.statusCode` | `301` | One of `301`, `302`, `307`, `308`. Use `308` (`http::StatusCodePermanentRedirect`) to make clients keep the method and body of non-`GET` requests. |

- The host is taken from the `Host` header, with its port replaced by the target port. A request without `Host` receives `400 Bad Request`.
- The path and query are preserved and re-encoded, so the `Location` value is always a valid URL, safe from header injection.
- The redirect bypasses routing, protocol upgrades, and request bodies, and closes the connection.
- Redirecting is a plaintext feature: it does not require OpenSSL, and enabling it together with TLS on the same listener fails validation.

In a configuration file:

```json
{
  "server": {
    "port": 80,
    "httpsRedirect": { "targetPort": 443, "statusCode": 308 }
  }
}
```

## Deployment checklist

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
- Publish rotated certificates, OCSP responses, and CRLs with atomic renames, reload them with `postConfigUpdate()`, and exercise it with invalid inputs before relying on automated rotation.

For the complete setting list, see [Server configuration](../reference/server-configuration.md#tls-configuration).
The [production configuration guide](../guides/production-configuration.md) covers listener-level deployment patterns.

## Tests

- Handshakes, mTLS, ALPN, SNI, and hot certificate reload (files rotated in place, symbolic link swap, rejected updates) against a live server: [tests/http-tls-handshake_test.cpp](../../tests/http-tls-handshake_test.cpp), and across `MultiHttpServer` workers: [tests/multi-http-server_test.cpp](../../tests/multi-http-server_test.cpp).
- Rollback of rejected config updates: [tests/http-additional_test.cpp](../../tests/http-additional_test.cpp).
- TLS reads and writes, read-ahead, and backpressure: [tests/http-tls-io_test.cpp](../../tests/http-tls-io_test.cpp).
- OCSP stapling, CRLs, the revocation callback, key logging, detection of changed input files, record coalescing, and kTLS send: [tls-components_test.cpp](../../aeronet/tls/test/tls-components_test.cpp).
- Session ticket key store and rotation: [tls-ticket-key-store_test.cpp](../../aeronet/tls/test/tls-ticket-key-store_test.cpp).
- Configuration validation and key scrubbing: [tls-config_test.cpp](../../aeronet/objects/test/tls-config_test.cpp).

Tests generate ephemeral self-signed certificates and pass them in memory with `withTlsCertKeyMemory()`, which avoids depending on files.
