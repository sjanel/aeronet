#pragma once

// Minimal reusable TLS client for tests (OpenSSL).
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "raw-bytes.hpp"
#include "test_util.hpp"

// Lightweight RAII TLS client used in tests to reduce duplication.
// Features:
//  * Automatic OpenSSL context + SSL object creation
//  * Optional ALPN protocol list (vector of protocol strings)
//  * Optional in-memory client certificate/key (PEM)
//  * Disable verification by default (tests use self-signed server certs)
//  * Simple helpers to GET a path and read full response
//  * Accessors for handshake success, negotiated ALPN
//
// Not intended for production usage; minimal error handling for brevity.
class TlsClient {
 public:
  struct Options {
    std::vector<std::string> alpn;  // e.g. {"http/1.1"}
    bool verifyPeer{false};         // off for self-signed tests
    std::string clientCertPem;      // optional client cert (mTLS)
    std::string clientKeyPem;       // optional client key (mTLS)
  };

  // Constructor without custom options.
  explicit TlsClient(uint16_t port) : _port(port), _opts() { init(); }

  // Constructor with explicit options.
  TlsClient(uint16_t port, Options options) : _port(port), _opts(std::move(options)) { init(); }

  TlsClient(const TlsClient&) = delete;
  TlsClient(TlsClient&&) noexcept = delete;
  TlsClient& operator=(const TlsClient&) = delete;
  TlsClient& operator=(TlsClient&&) noexcept = delete;

  ~TlsClient() {
    // Ensure orderly shutdown if possible
    if (_handshakeOk && _ssl) {
      ::SSL_shutdown(_ssl.get());
    }
  }

  [[nodiscard]] bool handshakeOk() const { return _handshakeOk; }

  // Send arbitrary bytes (only if handshake succeeded).
  bool writeAll(std::string_view data) {
    if (!_handshakeOk) {
      return false;
    }
    const char* cursor = data.data();
    auto remaining = data.size();
    while (remaining > 0) {
      int written = ::SSL_write(_ssl.get(), cursor, static_cast<int>(remaining));
      if (written <= 0) {
        int err = ::SSL_get_error(_ssl.get(), written);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
          continue;
        }
        return false;
      }
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
    }
    return true;
  }

  // Read until close (or error). Returns accumulated data.
  std::string readAll() {
    std::string out;
    if (!_handshakeOk) {
      return out;
    }
    char buf[4096];
    for (;;) {
      int bytesRead = ::SSL_read(_ssl.get(), buf, sizeof(buf));
      if (bytesRead > 0) {
        out.append(buf, buf + bytesRead);
        continue;
      }
      if (bytesRead == 0) {
        break;  // clean close
      }
      auto err = ::SSL_get_error(_ssl.get(), bytesRead);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        continue;
      }
      break;  // fatal
    }
    return out;
  }

  // Convenience: perform a GET request and read entire response.
  std::string get(const std::string& target,
                  const std::vector<std::pair<std::string, std::string>>& extraHeaders = {}) {
    if (!_handshakeOk) {
      return {};
    }
    std::string request = "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    for (const auto& header : extraHeaders) {
      request += header.first + ": " + header.second + "\r\n";
    }
    request += "\r\n";
    if (!writeAll(request)) {
      return {};
    }
    return readAll();
  }

  [[nodiscard]] std::string_view negotiatedAlpn() const { return _negotiatedAlpn; }

 private:
  using SSLUniquePtr = std::unique_ptr<SSL, void (*)(SSL*)>;
  using SSL_CTXUniquePtr = std::unique_ptr<SSL_CTX, void (*)(SSL_CTX*)>;

  static aeronet::RawBytes<unsigned char> buildAlpnWire(std::span<const std::string> protos) {
    aeronet::RawBytes<unsigned char> wire;
    for (const auto& protoStr : protos) {
      if (protoStr.size() > 255) {
        continue;  // skip invalid
      }
      wire.append(static_cast<unsigned char>(protoStr.size()));
      wire.append(reinterpret_cast<const unsigned char*>(protoStr.data()), protoStr.size());
    }
    return wire;
  }

  void init() {
    ::SSL_library_init();
    ::SSL_load_error_strings();
    SSL_CTXUniquePtr localCtx(::SSL_CTX_new(TLS_client_method()), ::SSL_CTX_free);
    if (!localCtx) {
      return;
    }
    if (!_opts.verifyPeer) {
      ::SSL_CTX_set_verify(localCtx.get(), SSL_VERIFY_NONE, nullptr);
    }
    if (!_opts.clientCertPem.empty() && !_opts.clientKeyPem.empty()) {
      loadClientCertKey(localCtx.get());
    }
    if (!_opts.alpn.empty()) {
      auto wire = buildAlpnWire(_opts.alpn);
      if (!wire.empty()) {
        ::SSL_CTX_set_alpn_protos(localCtx.get(), wire.data(), static_cast<unsigned int>(wire.size()));
      }
    }
    _cnx = ClientConnection(_port);

    SSLUniquePtr localSsl(::SSL_new(localCtx.get()), ::SSL_free);
    if (!localSsl) {
      throw std::runtime_error("Unable to allocate SSL");
    }
    ::SSL_set_fd(localSsl.get(), _cnx.fd());
    int rc = ::SSL_connect(localSsl.get());
    if (rc != 1) {
      _ctx = std::move(localCtx);
      _ssl = std::move(localSsl);
      _handshakeOk = false;
      return;
    }
    const unsigned char* proto = nullptr;
    unsigned int protoLen = 0;
    ::SSL_get0_alpn_selected(localSsl.get(), &proto, &protoLen);
    if (proto != nullptr && protoLen > 0) {
      _negotiatedAlpn.assign(reinterpret_cast<const char*>(proto), protoLen);
    }
    _handshakeOk = true;
    _ctx = std::move(localCtx);
    _ssl = std::move(localSsl);
  }

  void loadClientCertKey(SSL_CTX* ctx) {
    BIO* certBio = BIO_new_mem_buf(_opts.clientCertPem.data(), static_cast<int>(_opts.clientCertPem.size()));
    BIO* keyBio = BIO_new_mem_buf(_opts.clientKeyPem.data(), static_cast<int>(_opts.clientKeyPem.size()));
    if (certBio == nullptr || keyBio == nullptr) {
      if (certBio != nullptr) {
        BIO_free(certBio);
      }
      if (keyBio != nullptr) {
        BIO_free(keyBio);
      }
      return;
    }
    X509* cert = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
    if (cert != nullptr && pkey != nullptr) {
      ::SSL_CTX_use_certificate(ctx, cert);
      ::SSL_CTX_use_PrivateKey(ctx, pkey);
    }
    if (cert != nullptr) {
      X509_free(cert);
    }
    if (pkey != nullptr) {
      EVP_PKEY_free(pkey);
    }
    BIO_free(certBio);
    BIO_free(keyBio);
  }

  uint16_t _port;
  Options _opts;
  bool _handshakeOk{false};
  std::string _negotiatedAlpn;
  ClientConnection _cnx;
  // Initialize with valid deleter function pointers so default-constructed state is safe.
  SSL_CTXUniquePtr _ctx{nullptr, ::SSL_CTX_free};
  SSLUniquePtr _ssl{nullptr, ::SSL_free};
};
