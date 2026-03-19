#include "aeronet/test_tls_client.hpp"

#ifdef AERONET_POSIX
#include <fcntl.h>
#endif
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <openssl/types.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#ifdef AERONET_POSIX
#include <poll.h>
#elifdef AERONET_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
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

  // A single shared deadline for ALL WANT_WRITE waits within this writeAll()
  // call.  Giving each SSL_ERROR_WANT_WRITE a fresh deadline independently
  // causes an infinite loop when select() incorrectly reports write-readiness
  // on Windows loopback (a known platform bug similar to the WSAPoll one) but
  // SSL_write still returns WANT_WRITE: we retry immediately with a brand-new 5 s
  // budget, so we never actually give up.  Using one shared deadline guarantees
  // that the function eventually returns false if the write truly cannot make
  // progress.
  using Clock = std::chrono::steady_clock;
  auto wantWriteDeadline = Clock::now() + std::chrono::seconds(8);
  int sockfd = -1;  // lazily set on first WANT_WRITE

  while (remaining > 0) {
    int written = ::SSL_write(_ssl.get(), cursor, static_cast<int>(remaining));
    if (written > 0) {
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
      continue;
    }

    int err = ::SSL_get_error(_ssl.get(), written);
    if (err == SSL_ERROR_WANT_READ) {
      // SSL needs to read before it can write.  This can happen when OpenSSL
      // has already read TLS data off the socket into its internal buffer
      // during a previous operation (e.g., after the TLS handshake, the peer
      // sends its initial frames before we send ours).  In that case the
      // kernel socket buffer is empty so select/poll reports not-readable even
      // though data is available via SSL_read.  Always attempt SSL_read first
      // to drain any OpenSSL-buffered bytes; only block on the socket if
      // SSL_pending is zero (i.e., no data in OpenSSL's internal buffer).
      {
        char drainBuf[16384];
        int drained = ::SSL_read(_ssl.get(), drainBuf, static_cast<int>(sizeof(drainBuf)));
        if (drained > 0) {
          _drainedDuringWrite.append(drainBuf, static_cast<std::size_t>(drained));
          continue;  // retry SSL_write
        }
        int readErr = ::SSL_get_error(_ssl.get(), drained);
        if (readErr == SSL_ERROR_WANT_READ) {
          // Nothing buffered in OpenSSL; wait for kernel socket to become readable.
          // NOLINTNEXTLINE(misc-include-cleaner) header is <poll.h>, not <sys/poll.h>
          if (!waitForSocketReady(POLLIN, std::chrono::seconds(30))) {
            return false;
          }
        }
        // For any other error (WANT_WRITE etc.), just retry SSL_write.
        continue;
      }
    }
    if (err == SSL_ERROR_WANT_WRITE) {
      // SSL cannot flush to the socket yet. Poll for both POLLIN and POLLOUT:
      // if the peer is also blocked writing because our TCP receive buffer is
      // full, reading from the socket frees window and eventually unblocks our
      // send (breaking a bidirectional TCP deadlock).
      // SSL_read here is safe in TLS 1.3 — read/write paths are independent
      // and we always retry SSL_write with the same cursor/remaining below.
      if (sockfd < 0) {
        sockfd = ::SSL_get_fd(_ssl.get());
      }
      // Eagerly drain all available incoming data before polling.  Draining
      // frees TCP receive-window space so the peer can flush its send buffer,
      // which in turn lets the peer read our blocked bytes and free our TCP
      // send buffer.
      {
        char drainBuf[16384];
        for (;;) {
          int drained = ::SSL_read(_ssl.get(), drainBuf, static_cast<int>(sizeof(drainBuf)));
          if (drained <= 0) {
            break;
          }
          _drainedDuringWrite.append(drainBuf, static_cast<std::size_t>(drained));
        }
      }
      // Check the shared deadline AFTER draining so that as long as data
      // keeps flowing (drain succeeds) we don't give up prematurely.
      auto deadlineMs = std::chrono::duration_cast<std::chrono::milliseconds>(wantWriteDeadline - Clock::now()).count();
      if (deadlineMs <= 0) {
        return false;
      }
      // Wait for readability OR writability with a short timeout.
      // On Windows loopback, select() may spuriously report write-readiness;
      // we therefore do not exit the wait immediately on POLLOUT alone — we
      // always at least drain readable bytes first to help the peer make
      // progress before we try the write again.
      int pollMs = static_cast<int>(std::min(deadlineMs, static_cast<decltype(deadlineMs)>(100)));
      bool readable = false;
#ifdef AERONET_WINDOWS
      fd_set readFds;
      fd_set writeFds;
      FD_ZERO(&readFds);
      FD_ZERO(&writeFds);
      FD_SET(static_cast<SOCKET>(sockfd), &readFds);
      FD_SET(static_cast<SOCKET>(sockfd), &writeFds);
      struct timeval tv{};
      tv.tv_sec = pollMs / 1000;
      tv.tv_usec = (pollMs % 1000) * 1000;
      int sret = ::select(0, &readFds, &writeFds, nullptr, &tv);
      if (sret < 0) {
        return false;
      }
      if (sret > 0) {
        readable = FD_ISSET(static_cast<SOCKET>(sockfd), &readFds) != 0;
      }
#else
      struct pollfd wpfd{};  // NOLINT(misc-include-cleaner)
      wpfd.fd = sockfd;
      wpfd.events = POLLIN | POLLOUT;  // NOLINT(misc-include-cleaner)
      // NOLINTNEXTLINE(misc-include-cleaner)
      int wret = ::poll(&wpfd, 1, pollMs);
      if (wret < 0) {
        return false;
      }
      if (wret > 0) {
        readable = (wpfd.revents & POLLIN) != 0;  // NOLINT(misc-include-cleaner)
      }
#endif
      if (readable) {
        // Drain the newly arrived data to further free TCP receive window.
        char drainBuf[16384];
        for (;;) {
          int drained = ::SSL_read(_ssl.get(), drainBuf, static_cast<int>(sizeof(drainBuf)));
          if (drained <= 0) {
            break;
          }
          _drainedDuringWrite.append(drainBuf, static_cast<std::size_t>(drained));
        }
      }
      // Fall through to retry SSL_write regardless of whether select() reported
      // POLLOUT.  On platforms where POLLOUT reporting is reliable this avoids
      // an unnecessary extra 100 ms wait; on Windows loopback the retry is
      // harmless because another WANT_WRITE will simply bring us back here.
      continue;
    }
    // Fatal error
    char errBuf[256];
    ERR_error_string_n(ERR_get_error(), errBuf, sizeof(errBuf));
    log::error("SSL_write fatal error: {}, err={}", errBuf, err);
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
  // Maximum number of consecutive SSL_ERROR_SYSCALL+errno=0 retries before treating as EOF.
  // On Windows, non-blocking TLS sockets can transiently return SSL_ERROR_SYSCALL with no
  // system error before all application data has been delivered (see server-side handling in
  // tls-transport.cpp::read()). A short pause + retry avoids premature EOF on large responses.
  static constexpr int kMaxSyscallRetries = 5;
  int syscallRetries = 0;
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
      syscallRetries = 0;  // Reset retry counter on successful read
      continue;            // Successfully read data, try to read more
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
    if (err == SSL_ERROR_SYSCALL) {
      // On Windows, the server may close the connection without sending a TLS close_notify
      // (e.g., non-blocking SSL_shutdown could not flush). In that case SSL_read returns
      // SSL_ERROR_SYSCALL with no queued OpenSSL error and errno/WSAGetLastError() == 0.
      // Additionally, Windows non-blocking TLS can transiently produce this condition mid-
      // transfer before all data has been delivered. Retry a few times with a brief wait
      // (mirroring the server-side tls-transport.cpp::read() behaviour) before treating it
      // as a graceful EOF and returning the accumulated data.
      if (ERR_peek_error() == 0) {
        if (++syscallRetries <= kMaxSyscallRetries) {
          if (waitForSocketReady(POLLIN, std::chrono::milliseconds{100})) {
            continue;
          }
        }
        // Timed out or exceeded retries — treat as graceful EOF.
        break;
      }
      log::error("SSL_read SSL_ERROR_SYSCALL, OpenSSL error queue not empty");
    }
    // Fatal error
    break;
  }
  return out;
}

std::string TlsClient::peerCommonName() const {
  if (!_handshakeOk || !_ssl) {
    return {};
  }

  const X509* cert = ::SSL_get0_peer_certificate(_ssl.get());
  if (cert == nullptr) {
    return {};
  }
  X509_NAME* subj = ::X509_get_subject_name(const_cast<X509*>(cert));
  if (subj == nullptr) {
    return {};
  }

  char buf[256];
  const int len = ::X509_NAME_get_text_by_NID(subj, NID_commonName, buf, static_cast<int>(sizeof(buf)));
  if (len <= 0) {
    return {};
  }
  return {buf, static_cast<std::size_t>(len)};
}

std::string_view TlsClient::readSome(std::span<char> buffer) {
  if (!_handshakeOk || buffer.empty()) {
    return {};
  }

  int readRet = ::SSL_read(_ssl.get(), buffer.data(), static_cast<int>(buffer.size()));
  if (readRet > 0) {
    return {buffer.data(), static_cast<std::size_t>(readRet)};
  }

  int err = ::SSL_get_error(_ssl.get(), readRet);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    // No data available right now
    return {};
  }

  // Error or shutdown
  return {};
}

int TlsClient::fd() const noexcept {
  if (!_ssl) {
    return -1;
  }
  return ::SSL_get_fd(_ssl.get());
}

template <typename Duration>
bool TlsClient::waitForSocketReady(short events, Duration timeout) {
  int fd = ::SSL_get_fd(_ssl.get());
  if (fd < 0) {
    return false;
  }

  auto timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();

#ifdef AERONET_WINDOWS
  // Use select() instead of WSAPoll(): WSAPoll has a known Windows bug where
  // POLLOUT (and occasionally POLLIN) is not reliably reported on non-blocking
  // loopback sockets, causing spurious hangs.
  fd_set readFds{};
  fd_set writeFds{};
  FD_ZERO(&readFds);
  FD_ZERO(&writeFds);
  bool wantRead = (events & POLLIN) != 0;
  bool wantWrite = (events & POLLOUT) != 0;
  if (wantRead) {
    FD_SET(static_cast<SOCKET>(fd), &readFds);
  }
  if (wantWrite) {
    FD_SET(static_cast<SOCKET>(fd), &writeFds);
  }
  struct timeval tv{};
  tv.tv_sec = static_cast<long>(timeoutMs / 1000);
  tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
  int ret = ::select(0, wantRead ? &readFds : nullptr, wantWrite ? &writeFds : nullptr, nullptr, &tv);
  if (ret <= 0) {
    return false;
  }
  bool readable = wantRead && (FD_ISSET(static_cast<SOCKET>(fd), &readFds) != 0);
  bool writable = wantWrite && (FD_ISSET(static_cast<SOCKET>(fd), &writeFds) != 0);
  return readable || writable;
#else
  // NOLINTNEXTLINE(misc-include-cleaner) header is <poll.h>, not <sys/poll.h>
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = events;
  pfd.revents = 0;
  // NOLINTNEXTLINE(misc-include-cleaner) header is <poll.h>, not <sys/poll.h>
  int ret = ::poll(&pfd, 1, static_cast<int>(timeoutMs));
  if (ret < 0) {
    return false;
  }
  if (ret == 0) {
    return false;
  }
  return (pfd.revents & events) != 0;
#endif
}

// Convenience: perform a GET request and read entire response.
std::string TlsClient::get(std::string_view target, const std::vector<http::Header>& extraHeaders) {
  if (!_handshakeOk) {
    return {};
  }
  std::string request = "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
  for (const auto& header : extraHeaders) {
    request.append(header.name()).append(http::HeaderSep).append(header.value()).append(http::CRLF);
  }
  request += http::CRLF;
  if (!writeAll(request)) {
    return {};
  }
  return readAll();
}

void TlsClient::init() {
  // Avoid test process termination when writing to a closed TLS socket.
  // OpenSSL ultimately writes to the underlying fd, which can raise SIGPIPE on Linux.
#ifdef AERONET_POSIX
  static const int kSigpipeIgnored = []() {
    ::signal(SIGPIPE, SIG_IGN);  // NOLINT(misc-include-cleaner)
    return 0;
  }();
  (void)kSigpipeIgnored;
#endif

  ::SSL_library_init();
  ::SSL_load_error_strings();
  SSL_CTXUniquePtr localCtx(::SSL_CTX_new(TLS_client_method()), ::SSL_CTX_free);
  if (!localCtx) {
    return;
  }

  // Ensure TLS session resumption works consistently across OpenSSL builds.
  // Some libssl configurations default to a disabled/limited client-side session cache,
  // which prevents TLS 1.3 NewSessionTicket from being retained for later reuse.
  ::SSL_CTX_set_session_cache_mode(localCtx.get(), SSL_SESS_CACHE_CLIENT);

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
  auto fd = _cnx.fd();
#ifdef AERONET_WINDOWS
  u_long nonBlock = 1;
  ::ioctlsocket(fd, FIONBIO, &nonBlock);
#else
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
#endif

  SSLUniquePtr localSsl(::SSL_new(localCtx.get()), ::SSL_free);
  if (!localSsl) {
    throw std::runtime_error("Unable to allocate SSL");
  }
  // OpenSSL's SSL_set_fd takes int; on Windows NativeHandle is SOCKET (UINT_PTR) but the
  // value round-trips safely through int for sockets allocated by the OS.
  ::SSL_set_fd(localSsl.get(), static_cast<int>(fd));
  if (!_opts.serverName.empty()) {
    ::SSL_set_tlsext_host_name(localSsl.get(), _opts.serverName.c_str());
  }
  if (_opts.reuseSession != nullptr) {
    // Best-effort: if it fails, handshake will proceed as full.
    (void)::SSL_set_session(localSsl.get(), _opts.reuseSession);
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

TlsClient::SessionUniquePtr TlsClient::get1Session() const {
  if (!_handshakeOk || !_ssl) {
    return {nullptr, ::SSL_SESSION_free};
  }
  SSL_SESSION* sess = ::SSL_get1_session(_ssl.get());
  return {sess, ::SSL_SESSION_free};
}

void TlsClient::loadClientCertKey(SSL_CTX* ctx) {
  BioPtr certBio(BIO_new_mem_buf(_opts.clientCertPem.data(), static_cast<int>(_opts.clientCertPem.size())), ::BIO_free);
  BioPtr keyBio(BIO_new_mem_buf(_opts.clientKeyPem.data(), static_cast<int>(_opts.clientKeyPem.size())), ::BIO_free);
  if (!certBio || !keyBio) {
    return;  // allocation failure
  }
  X509Ptr cert(PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), ::X509_free);
  PKeyPtr pkey(PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), ::EVP_PKEY_free);
  if (cert != nullptr && pkey != nullptr) {
    ::SSL_CTX_use_certificate(ctx, cert.get());
    ::SSL_CTX_use_PrivateKey(ctx, pkey.get());
  }
}

void TlsClient::loadTrustedServerCert(SSL_CTX* ctx) {
  BioPtr caBio(BIO_new_mem_buf(_opts.trustedServerCertPem.data(), static_cast<int>(_opts.trustedServerCertPem.size())),
               ::BIO_free);
  if (!caBio) {
    return;
  }
  X509Ptr ca(PEM_read_bio_X509(caBio.get(), nullptr, nullptr, nullptr), ::X509_free);
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
