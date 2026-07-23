#include "aeronet/http-client-tls-context.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/prov_ssl.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <openssl/types.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/client-protocol.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-exception.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-raii.hpp"
#include "aeronet/tls-transport.hpp"
#include "aeronet/transport.hpp"

namespace aeronet::internal {

namespace {

// Map an aeronet TLSConfig::Version onto the OpenSSL protocol constant (0 == unset / unsupported).
constexpr int ToOpenSslTlsVersion(TLSConfig::Version ver) {
  if (ver == TLSConfig::TLS_1_2) {
    return TLS1_2_VERSION;
  }
#ifdef TLS1_3_VERSION
  if (ver == TLSConfig::TLS_1_3) {
    return TLS1_3_VERSION;
  }
#endif
  return 0;
}

// Load a client certificate + private key (mutual TLS) into the context, from in-memory PEM if
// provided, otherwise from file paths. No-op when neither is configured.
void LoadClientCertificate(SSL_CTX* ctx, const HttpClientConfig& cfg) {
  const std::string_view certPem = cfg.tlsClientCertPem();
  const std::string_view keyPem = cfg.tlsClientKeyPem();
  if (!certPem.empty() && !keyPem.empty()) {
    auto certBio = MakeMemBio(certPem.data(), static_cast<int>(certPem.size()));
    auto keyBio = MakeMemBio(keyPem.data(), static_cast<int>(keyPem.size()));
    // Wrap the parse results directly (rather than via MakeX509/MakePKey, which raise std::bad_alloc on
    // null) so a malformed PEM surfaces as the documented HttpClientException, not a misleading bad_alloc.
    X509Ptr certX509(::PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), ::X509_free);
    PKeyPtr pkey(::PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), ::EVP_PKEY_free);
    if (!certX509 || !pkey) {
      throw HttpClientException("Failed to parse in-memory client certificate or key");
    }
    if (::SSL_CTX_use_certificate(ctx, certX509.get()) != 1 || ::SSL_CTX_use_PrivateKey(ctx, pkey.get()) != 1) {
      throw HttpClientException("Failed to install in-memory client certificate");
    }
  } else if (!cfg.tlsClientCertFile().empty() && !cfg.tlsClientKeyFile().empty()) {
    if (::SSL_CTX_use_certificate_file(ctx, cfg.tlsClientCertFileCStr(), SSL_FILETYPE_PEM) != 1) {
      throw HttpClientException("Failed to load client certificate file");
    }
    if (::SSL_CTX_use_PrivateKey_file(ctx, cfg.tlsClientKeyFileCStr(), SSL_FILETYPE_PEM) != 1) {
      throw HttpClientException("Failed to load client private key file");
    }
  } else {
    return;  // no client certificate configured
  }
  if (::SSL_CTX_check_private_key(ctx) != 1) {
    throw HttpClientException("Client private key does not match the certificate");
  }
}

// Well-known Linux system CA bundle files probed (in priority order) when the caller configures no explicit trust
// store and the environment does not already point OpenSSL at one. This is the same set curl / Go / Python
// fall back to. OpenSSL's own SSL_CTX_set_default_verify_paths() only consults its compiled-in directory
// (often /usr/lib/ssl) plus the SSL_CERT_FILE / SSL_CERT_DIR env vars, and that directory is frequently
// absent from minimal container images that ship just /etc/ssl/certs/ca-certificates.crt -- leaving the
// store empty and every verification failing with "certificate verify failed".
constexpr const char* const kDefaultCaBundleFiles[] = {
    "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu, Gentoo, Arch, Alpine
    "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora, RHEL, CentOS
    "/etc/ssl/ca-bundle.pem",              // openSUSE
    "/etc/pki/tls/cacert.pem",             // OpenELEC / old RHEL
    "/etc/ssl/cert.pem",                   // Alpine, FreeBSD, macOS (LibreSSL)
};

// Well-known hashed-cert directories probed as a fallback in the same situation.
constexpr const char* const kDefaultCaBundleDirs[] = {
    "/etc/ssl/certs",                // Debian, Ubuntu, SUSE
    "/etc/pki/tls/certs",            // Fedora, RHEL
    "/system/etc/security/cacerts",  // Android
};

// True when the environment already points OpenSSL at a trust store through SSL_CERT_FILE / SSL_CERT_DIR
// (the env var names are queried from OpenSSL itself). In that case SSL_CTX_set_default_verify_paths()
// honours it, so aeronet must not widen that (possibly deliberately restricted) trust set by loading the
// well-known system bundles on top of it.
bool SystemTrustStoreConfiguredViaEnv() {
  const char* envNames[] = {::X509_get_default_cert_file_env(), ::X509_get_default_cert_dir_env()};
  return std::ranges::any_of(envNames, [](const char* envName) {
    const char* pEnv = std::getenv(envName);
    return pEnv != nullptr && *pEnv != '\0';
  });
}

}  // namespace

bool LoadExistingCaBundles(void* sslCtx, std::span<const char* const> caFiles, std::span<const char* const> caDirs) {
  auto* ctx = static_cast<SSL_CTX*>(sslCtx);
  std::error_code ec;
  bool loaded = false;
  for (const char* caFile : caFiles) {
    if (std::filesystem::is_regular_file(caFile, ec) && ::SSL_CTX_load_verify_locations(ctx, caFile, nullptr) == 1) {
      loaded = true;
    }
  }
  for (const char* caDir : caDirs) {
    if (std::filesystem::is_directory(caDir, ec) && ::SSL_CTX_load_verify_locations(ctx, nullptr, caDir) == 1) {
      loaded = true;
    }
  }
  return loaded;
}

HttpClientTlsContext::HttpClientTlsContext(const HttpClientConfig& cfg) {
  cfg.validate();
  // Own the SSL_CTX through an RAII guard for the whole setup: any step below may throw (bad cipher list,
  // unusable cert/key, ...) and the guard frees the context on the way out. Ownership is transferred to
  // pCtx only once every step has succeeded, so a failed build never leaks the SSL_CTX.
  SslCtxPtr ctx(::SSL_CTX_new(::TLS_client_method()), &::SSL_CTX_free);
  if (!ctx) {
    throw HttpClientException("SSL_CTX_new(TLS_client_method) failed");
  }
  SSL_CTX* pSsl = ctx.get();
  const int minVersion = ToOpenSslTlsVersion(cfg.tlsMinVersion);
  if (minVersion != 0 && ::SSL_CTX_set_min_proto_version(pSsl, minVersion) != 1) {
    throw HttpClientException("Failed to set minimum TLS version");
  }
  if (cfg.tlsMaxVersion != TLSConfig::Version{}) {
    const int maxVersion = ToOpenSslTlsVersion(cfg.tlsMaxVersion);
    if (maxVersion == 0 || ::SSL_CTX_set_max_proto_version(pSsl, maxVersion) != 1) {
      throw HttpClientException("Failed to set maximum TLS version");
    }
  }
  if (!cfg.tlsCipherList().empty() && ::SSL_CTX_set_cipher_list(pSsl, cfg.tlsCipherListCStr()) != 1) {
    throw HttpClientException("Failed to set TLS cipher list");
  }
  LoadClientCertificate(pSsl, cfg);
  ::SSL_CTX_set_mode(pSsl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  // Advertise the application protocols we can actually speak via ALPN (OpenSSL's length-prefixed wire
  // format), driven by cfg.httpVersion: "h2" is offered first (preferred) in Auto, alone in Http2, and
  // not at all in Http1_1 or a build without HTTP/2 support. The negotiated protocol is read back from
  // SSL_get0_alpn_selected() in finishConnect(). SSL_CTX_set_alpn_protos returns 0 on success.
  static constexpr unsigned char kAlpnHttp11[]{8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
#ifdef AERONET_ENABLE_HTTP2
  static constexpr unsigned char kAlpnH2Http11[]{2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  static constexpr unsigned char kAlpnH2[]{2, 'h', '2'};
  std::span<const unsigned char> alpnWire(kAlpnHttp11);
  if (cfg.httpVersion == HttpVersionMode::Auto) {
    alpnWire = kAlpnH2Http11;
  } else if (cfg.httpVersion == HttpVersionMode::Http2) {
    alpnWire = kAlpnH2;
  }
#else
  const std::span<const unsigned char> alpnWire(kAlpnHttp11);
#endif
  if (::SSL_CTX_set_alpn_protos(pSsl, alpnWire.data(), static_cast<unsigned int>(alpnWire.size())) != 0) {
    throw HttpClientException("Failed to set TLS ALPN protocols");
  }
  if (cfg.tlsVerifyPeer) {
    ::SSL_CTX_set_verify(pSsl, SSL_VERIFY_PEER, nullptr);
    bool trustLoaded = false;
    // A forward proxy that intercepts TLS re-signs origin certificates with its own CA: verify against
    // that CA (proxyCaFile) in preference to the general tlsCaFile / default trust store.
    const char* caFile = nullptr;
    if (!cfg.proxyCaFile().empty()) {
      caFile = cfg.proxyCaFileCStr();
    } else if (!cfg.tlsCaFile().empty()) {
      caFile = cfg.tlsCaFileCStr();
    }
    const char* caPath = cfg.tlsCaPath().empty() ? nullptr : cfg.tlsCaPathCStr();
    std::error_code caPathError;
    if (caPath != nullptr && !std::filesystem::is_directory(caPath, caPathError)) {
      throw HttpClientException("TLS CA path does not exist or is not a directory");
    }
    if (caFile != nullptr || caPath != nullptr) {
      trustLoaded = ::SSL_CTX_load_verify_locations(pSsl, caFile, caPath) == 1;
      if (!trustLoaded) {
        throw HttpClientException("Failed to load TLS CA trust store");
      }
    } else {
      // No explicit trust store configured: fall back to the system's. OpenSSL's default resolution
      // honours the SSL_CERT_FILE / SSL_CERT_DIR environment variables and its compiled-in default paths.
      trustLoaded = ::SSL_CTX_set_default_verify_paths(pSsl) == 1;
      // OpenSSL's compiled-in default directory (often /usr/lib/ssl) is frequently absent from minimal
      // container images that ship only a bundle such as /etc/ssl/certs/ca-certificates.crt, which would
      // otherwise leave the store empty and fail every verification. Unless the environment already points
      // OpenSSL at a store, augment it with any well-known system CA location that exists (best effort).
      if (!SystemTrustStoreConfiguredViaEnv()) {
        trustLoaded = LoadExistingCaBundles(pSsl, kDefaultCaBundleFiles, kDefaultCaBundleDirs) || trustLoaded;
      }
      if (!trustLoaded) {
        throw HttpClientException("Failed to load default TLS trust store");
      }
    }
  } else {
    ::SSL_CTX_set_verify(pSsl, SSL_VERIFY_NONE, nullptr);
  }
  // All setup succeeded: take ownership of the fully-built context.
  pCtx = ctx.release();
}

// Build a TLS transport in client connect state, with SNI and (optionally) hostname verification.
[[nodiscard]] std::unique_ptr<ITransport> HttpClientTlsContext::makeTransport(NativeHandle fd, const char* pHost,
                                                                              bool verify) const {
  TlsTransport::SslPtr ssl(::SSL_new(static_cast<SSL_CTX*>(pCtx)), &::SSL_free);
  if (!ssl) {
    throw HttpClientException("SSL_new failed");
  }
  if (::SSL_set_fd(ssl.get(), static_cast<int>(fd)) != 1) {
    throw HttpClientException("SSL_set_fd failed");
  }
  // OpenSSL SNI / verification APIs need a null-terminated host string.
  // SNI (host must be a registered name, not an IP literal; OpenSSL ignores IPs here which is fine).
  ::SSL_set_tlsext_host_name(ssl.get(), pHost);
  if (verify) {
    ::SSL_set_hostflags(ssl.get(), X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (::SSL_set1_host(ssl.get(), pHost) != 1) {
      throw HttpClientException("SSL_set1_host failed");
    }
  }
  ::SSL_set_connect_state(ssl.get());
  auto transport = std::make_unique<TlsTransport>(std::move(ssl), ~0U);
  transport->setUnderlyingFd(fd);
  return transport;
}

HttpClientTlsContext::HttpClientTlsContext(HttpClientTlsContext&& rhs) noexcept
    : pCtx(std::exchange(rhs.pCtx, nullptr)) {}

HttpClientTlsContext& HttpClientTlsContext::operator=(HttpClientTlsContext&& rhs) noexcept {
  if (this != &rhs) {
    ::SSL_CTX_free(static_cast<SSL_CTX*>(pCtx));
    pCtx = std::exchange(rhs.pCtx, nullptr);
  }
  return *this;
}

HttpClientTlsContext::~HttpClientTlsContext() { ::SSL_CTX_free(static_cast<SSL_CTX*>(pCtx)); }

}  // namespace aeronet::internal