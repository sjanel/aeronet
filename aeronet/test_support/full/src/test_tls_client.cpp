#include "aeronet/test_tls_client.hpp"

#include <fcntl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <openssl/types.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/tls-raii.hpp"

namespace aeronet::test {

namespace {
RawBytes BuildAlpnWire(std::span<const std::string> protos) {
  RawBytes wire;
  for (const auto& protoStr : protos) {
    wire.push_back(static_cast<std::byte>(protoStr.size()));
    wire.append(reinterpret_cast<const std::byte*>(protoStr.data()), protoStr.size());
  }
  return wire;
}
}  // namespace

TlsClient::~TlsClient() {
  // Ensure orderly shutdown if possible
  if (_handshakeOk && _ssl) {
    ::SSL_shutdown(_ssl.get());
  }
}

bool TlsClient::writeAll(std::string_view data) {
  if (!_handshakeOk) {
    return false;
  }
  const char* cursor = data.data();
  auto remaining = data.size();

  while (remaining > 0) {
    int written = ::SSL_write(_ssl.get(), cursor, static_cast<int>(remaining));
    if (written > 0) {
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
      continue;
    }

    int err = ::SSL_get_error(_ssl.get(), written);
    if (err == SSL_ERROR_WANT_READ) {
      // SSL needs to read before it can write (e.g., renegotiation)
      // NOLINTNEXTLINE(misc-include-cleaner) header is <poll.h>, not <sys/poll.h>
      if (!waitForSocketReady(POLLIN, std::chrono::seconds(30))) {
        return false;
      }
      continue;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
      // SSL needs socket to be writable
      // NOLINTNEXTLINE(misc-include-cleaner) header is <poll.h>, not <sys/poll.h>
      if (!waitForSocketReady(POLLOUT, std::chrono::seconds(30))) {
        return false;
      }
      continue;
    }
    // Fatal error
    return false;
  }
  return true;
}

std::string TlsClient::readAll() {
  std::string out;
  if (!_handshakeOk) {
    return out;
  }
  static constexpr std::size_t kChunkSize = 4096;
  out.reserve(kChunkSize);

  for (;;) {
    const auto oldSize = out.size();
    if (out.capacity() < out.size() + kChunkSize) {
      // ensure exponential growth
      out.reserve(out.capacity() * 2UL);
    }

    int readRet = 0;
    out.resize_and_overwrite(out.size() + kChunkSize,
                             [this, oldSize, &readRet](char* data, [[maybe_unused]] std::size_t newCap) {
                               readRet = ::SSL_read(_ssl.get(), data + oldSize, kChunkSize);
                               if (readRet > 0) {
                                 return oldSize + static_cast<std::size_t>(readRet);
                               }
                               return oldSize;
                             });

    if (readRet > 0) {
      continue;  // Successfully read data, try to read more
    }

    // readRet <= 0: check SSL error
    int err = ::SSL_get_error(_ssl.get(), readRet);
    if (err == SSL_ERROR_ZERO_RETURN) {
      // Clean SSL shutdown
      break;
    }
    if (err == SSL_ERROR_WANT_READ) {
      // SSL needs socket to be readable
      if (!waitForSocketReady(POLLIN, std::chrono::seconds(30))) {
        break;  // Timeout or error
      }
      continue;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
      // SSL needs to write before it can read (e.g., renegotiation)
      if (!waitForSocketReady(POLLOUT, std::chrono::seconds(30))) {
        break;  // Timeout or error
      }
      continue;
    }
    // Fatal error
    break;
  }
  return out;
}

template <typename Duration>
bool TlsClient::waitForSocketReady(short events, Duration timeout) {
  int fd = ::SSL_get_fd(_ssl.get());
  if (fd < 0) {
    return false;
  }

  // NOLINTNEXTLINE(misc-include-cleaner) header is <poll.h>, not <sys/poll.h>
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = events;
  pfd.revents = 0;

  auto timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
  // NOLINTNEXTLINE(misc-include-cleaner) header is <poll.h>, not <sys/poll.h>
  int ret = ::poll(&pfd, 1, static_cast<int>(timeoutMs));

  if (ret < 0) {
    // poll error
    return false;
  }
  if (ret == 0) {
    // Timeout
    return false;
  }

  // Check if the requested event occurred
  return (pfd.revents & events) != 0;
}

// Convenience: perform a GET request and read entire response.
std::string TlsClient::get(std::string_view target, const std::vector<http::Header>& extraHeaders) {
  if (!_handshakeOk) {
    return {};
  }
  std::string request = "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
  for (const auto& header : extraHeaders) {
    request.append(header.name()).append(aeronet::http::HeaderSep).append(header.value()).append(aeronet::http::CRLF);
  }
  request += aeronet::http::CRLF;
  if (!writeAll(request)) {
    return {};
  }
  return readAll();
}

void TlsClient::init() {
  ::SSL_library_init();
  ::SSL_load_error_strings();
  SSL_CTXUniquePtr localCtx(::SSL_CTX_new(TLS_client_method()), ::SSL_CTX_free);
  if (!localCtx) {
    return;
  }
  if (_opts.verifyPeer) {
    ::SSL_CTX_set_verify(localCtx.get(), SSL_VERIFY_PEER, nullptr);
    if (!_opts.trustedServerCertPem.empty()) {
      loadTrustedServerCert(localCtx.get());
    }
  } else {
    ::SSL_CTX_set_verify(localCtx.get(), SSL_VERIFY_NONE, nullptr);
  }
  if (!_opts.clientCertPem.empty() && !_opts.clientKeyPem.empty()) {
    loadClientCertKey(localCtx.get());
  }
  if (!_opts.alpn.empty()) {
    auto wire = BuildAlpnWire(_opts.alpn);
    if (!wire.empty()) {
      ::SSL_CTX_set_alpn_protos(localCtx.get(), reinterpret_cast<const unsigned char*>(wire.data()),
                                static_cast<unsigned int>(wire.size()));
    }
  }

  // Set socket to non-blocking mode for poll()-based I/O
  int fd = _cnx.fd();
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  SSLUniquePtr localSsl(::SSL_new(localCtx.get()), ::SSL_free);
  if (!localSsl) {
    throw std::runtime_error("Unable to allocate SSL");
  }
  ::SSL_set_fd(localSsl.get(), fd);
  if (!_opts.serverName.empty()) {
    ::SSL_set_tlsext_host_name(localSsl.get(), _opts.serverName.c_str());
  }

  // Move ownership first so we can use waitForSocketReady
  _ctx = std::move(localCtx);
  _ssl = std::move(localSsl);

  // Perform SSL handshake with non-blocking socket
  for (;;) {
    int rc = ::SSL_connect(_ssl.get());
    if (rc == 1) {
      // Handshake successful
      break;
    }

    int err = ::SSL_get_error(_ssl.get(), rc);
    if (err == SSL_ERROR_WANT_READ) {
      if (!waitForSocketReady(POLLIN, std::chrono::seconds(30))) {
        _handshakeOk = false;
        return;
      }
      continue;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
      if (!waitForSocketReady(POLLOUT, std::chrono::seconds(30))) {
        _handshakeOk = false;
        return;
      }
      continue;
    }
    // Fatal error
    // Log OpenSSL error queue for diagnostics
    auto err2 = ERR_get_error();
    if (err2 != 0) {
      char buf[256];
      ERR_error_string_n(err2, buf, sizeof(buf));
      log::error("Client TLS handshake fatal error: {}", buf);
      while ((err2 = ERR_get_error()) != 0) {
        ERR_error_string_n(err2, buf, sizeof(buf));
        log::error("Client TLS handshake fatal error: {}", buf);
      }
    }
    _handshakeOk = false;
    return;
  }
  const unsigned char* proto = nullptr;
  unsigned int protoLen = 0;
  ::SSL_get0_alpn_selected(_ssl.get(), &proto, &protoLen);
  if (proto != nullptr) {
    _negotiatedAlpn.assign(reinterpret_cast<const char*>(proto), protoLen);
  }
  _handshakeOk = true;
  // Log negotiated values for debugging
  {
    const char* ver = ::SSL_get_version(_ssl.get());
    const char* cipher = ::SSL_get_cipher_name(_ssl.get());
    log::info("Client negotiated TLS ver={} cipher={} alpn={}", (ver != nullptr) ? ver : "?",
              (cipher != nullptr) ? cipher : "?", _negotiatedAlpn.empty() ? "-" : _negotiatedAlpn);
  }
}

void TlsClient::loadClientCertKey(SSL_CTX* ctx) {
  aeronet::BioPtr certBio(BIO_new_mem_buf(_opts.clientCertPem.data(), static_cast<int>(_opts.clientCertPem.size())),
                          ::BIO_free);
  aeronet::BioPtr keyBio(BIO_new_mem_buf(_opts.clientKeyPem.data(), static_cast<int>(_opts.clientKeyPem.size())),
                         ::BIO_free);
  if (!certBio || !keyBio) {
    return;  // allocation failure
  }
  aeronet::X509Ptr cert(PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), ::X509_free);
  aeronet::PKeyPtr pkey(PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), ::EVP_PKEY_free);
  if (cert != nullptr && pkey != nullptr) {
    ::SSL_CTX_use_certificate(ctx, cert.get());
    ::SSL_CTX_use_PrivateKey(ctx, pkey.get());
  }
}

void TlsClient::loadTrustedServerCert(SSL_CTX* ctx) {
  aeronet::BioPtr caBio(
      BIO_new_mem_buf(_opts.trustedServerCertPem.data(), static_cast<int>(_opts.trustedServerCertPem.size())),
      ::BIO_free);
  if (!caBio) {
    return;
  }
  aeronet::X509Ptr ca(PEM_read_bio_X509(caBio.get(), nullptr, nullptr, nullptr), ::X509_free);
  if (!ca) {
    return;
  }
  auto* store = ::SSL_CTX_get_cert_store(ctx);
  if (store == nullptr) {
    return;
  }
  // Ignore duplicate insertion errors (multiple tests may reuse same PEM)
  ::X509_STORE_add_cert(store, ca.get());
}

}  // namespace aeronet::test