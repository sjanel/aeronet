#pragma once

#include <memory>
#include <span>

#include "aeronet/http-client-config.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/transport.hpp"

namespace aeronet::internal {

// OpenSSL client context: one SSL_CTX shared by all connections of a single HttpClient.
// Default-constructed empty (no SSL_CTX allocated); built lazily on the first https request so a
// plain-http client never pays for an OpenSSL context and a bad TLS configuration surfaces as an
// HttpClientException at first-request time rather than at client construction.
class HttpClientTlsContext {
 public:
  // Empty context (no SSL_CTX). Build one with the cfg constructor when an https request is first made.
  HttpClientTlsContext() noexcept = default;

  explicit HttpClientTlsContext(const HttpClientConfig& cfg);

  HttpClientTlsContext(const HttpClientTlsContext&) = delete;
  HttpClientTlsContext& operator=(const HttpClientTlsContext&) = delete;

  HttpClientTlsContext(HttpClientTlsContext&& rhs) noexcept;
  HttpClientTlsContext& operator=(HttpClientTlsContext&& rhs) noexcept;

  ~HttpClientTlsContext();

  // Whether the underlying SSL_CTX has not been built yet.
  [[nodiscard]] bool empty() const noexcept { return pCtx == nullptr; }

  // Build a TLS transport in client connect state, with SNI and (optionally) hostname verification.
  [[nodiscard]] std::unique_ptr<ITransport> makeTransport(NativeHandle fd, const char* pHost, bool verify) const;

 private:
  void* pCtx{};
};

// Best-effort augmentation of an OpenSSL trust store: load into `sslCtx` (an `SSL_CTX*`, taken as `void*`
// to keep OpenSSL out of this header) every CA bundle file in `caFiles` and every hashed-cert directory in
// `caDirs` that currently exists on disk. Returns true iff at least one location was loaded. aeronet uses
// this to complement OpenSSL's own default trust store with well-known system locations, so HTTPS
// verification works out of the box on minimal container images whose OpenSSL build points its compiled-in
// default directory at a path they do not ship. Exposed (rather than translation-unit-local) so it can be
// unit-tested with injected paths.
[[nodiscard]] bool LoadExistingCaBundles(void* sslCtx, std::span<const char* const> caFiles,
                                         std::span<const char* const> caDirs);

}  // namespace aeronet::internal