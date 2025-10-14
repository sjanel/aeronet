#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-server.hpp"
#include "connection-state.hpp"
#include "connection.hpp"
#include "raw-chars.hpp"
#include "transport.hpp"
#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/types.h>

#include "tls-context.hpp"
#include "tls-handshake.hpp"
#include "tls-raii.hpp"
#include "tls-transport.hpp"  // from tls module include directory
#endif
#include "event-loop.hpp"
#include "log.hpp"

namespace aeronet {

void HttpServer::sweepIdleConnections() {
  // Periodic maintenance of live connections: applies keep-alive timeout (if enabled) and
  // header read timeout (always, regardless of keep-alive enablement). The header read timeout
  // needs a periodic check because a client might send a partial request line then stall; no
  // further EPOLLIN events will arrive to trigger enforcement in handleReadableClient().
  auto now = std::chrono::steady_clock::now();
  for (auto cnxIt = _connStates.begin(); cnxIt != _connStates.end();) {
    ConnectionState& st = cnxIt->second;
    // Keep-alive inactivity enforcement only if enabled.
    if (_config.enableKeepAlive) {
      if (st.isAnyCloseRequested() || (now - st.lastActivity) > _config.keepAliveTimeout) {
        cnxIt = closeConnection(cnxIt);
        continue;
      }
    } else if (st.isAnyCloseRequested()) {
      cnxIt = closeConnection(cnxIt);
      continue;
    }
    // Header read timeout: active if headerStart set and duration exceeded and no full request parsed yet.
    if (_config.headerReadTimeout.count() > 0 && st.headerStart.time_since_epoch().count() != 0) {
      if (now - st.headerStart > _config.headerReadTimeout) {
        cnxIt = closeConnection(cnxIt);
        continue;
      }
    }
    // TLS handshake timeout (if enabled). Applies only while handshake pending.
#ifdef AERONET_ENABLE_OPENSSL
    if (_config.tlsHandshakeTimeout.count() > 0 && _config.tls && st.handshakeStart.time_since_epoch().count() != 0 &&
        !st.tlsEstablished && !st.transport->handshakeDone()) {
      if (now - st.handshakeStart > _config.tlsHandshakeTimeout) {
        cnxIt = closeConnection(cnxIt);
        continue;
      }
    }
#endif
    ++cnxIt;
  }
}

void HttpServer::acceptNewConnections() {
  while (true) {
    Connection cnx(_listenSocket);
    if (!cnx.isOpened()) {
      // no more waiting connections
      break;
    }
    int cnxFd = cnx.fd();
    if (!_eventLoop.add(cnxFd, EPOLLIN | EPOLLET)) {
      auto savedErr = errno;
      log::error("EventLoop add client failed fd={} err={}: {}", cnxFd, savedErr, std::strerror(savedErr));
      continue;
    }
    auto [cnxIt, inserted] = _connStates.emplace(std::move(cnx), ConnectionState{});
    if (!inserted) {
      log::error("Internal error: accepted connection fd={} already present in connection map", cnxFd);
      // Close the newly accepted connection immediately to avoid fd leak.
      _eventLoop.del(cnxFd);
      continue;
    }

    // Track new connection acceptance
    _telemetry.counterAdd("aeronet.connections.accepted", 1UL);

    ConnectionState& state = cnxIt->second;
#ifdef AERONET_ENABLE_OPENSSL
    if (_tlsCtxHolder) {
      SSL_CTX* ctx = reinterpret_cast<SSL_CTX*>(_tlsCtxHolder->raw());
      SslPtr sslPtr(SSL_new(ctx), SSL_free);
      if (sslPtr.get() == nullptr) {
        continue;
      }

      if (SSL_set_fd(sslPtr.get(), cnxFd) != 1) {  // associate
        log::error("SSL_set_fd failed for fd={}", cnxFd);
        continue;
      }
      // Enable partial writes: SSL_write will return after writing some data rather than
      // trying to write everything. This is crucial for non-blocking I/O performance.
      SSL_set_mode(sslPtr.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
      SSL_set_accept_state(sslPtr.get());
      state.transport = std::make_unique<TlsTransport>(std::move(sslPtr));
      state.handshakeStart = std::chrono::steady_clock::now();
    } else {
      state.transport = std::make_unique<PlainTransport>(cnxFd);
    }
#else
    state.transport = std::make_unique<PlainTransport>(cnxFd);
#endif
    ConnectionState* pCnx = &state;
    std::size_t bytesReadThisEvent = 0;
    while (true) {
      // Determine adaptive chunk size: if we have not yet parsed a full header for the current pending request
      // (heuristic: headerStart set OR buffer missing CRLFCRLF), use initialReadChunkBytes; otherwise use
      // bodyReadChunkBytes.
      std::size_t chunkSize = _config.bodyReadChunkBytes;
      if (pCnx->headerStart.time_since_epoch().count() != 0 ||
          std::ranges::search(pCnx->buffer, http::DoubleCRLF).empty()) {
        chunkSize = _config.initialReadChunkBytes;
      }
      if (_config.maxPerEventReadBytes != 0) {
        std::size_t remainingBudget = (_config.maxPerEventReadBytes > bytesReadThisEvent)
                                          ? (_config.maxPerEventReadBytes - bytesReadThisEvent)
                                          : 0;
        if (remainingBudget == 0) {
          break;  // fairness cap reached for this epoll cycle
        }
        chunkSize = std::min(chunkSize, remainingBudget);
      }
      Transport want;
      const std::size_t bytesRead = pCnx->transportRead(chunkSize, want);
      // Check for handshake completion
      // If the TLS handshake completed during the preceding transportRead, finalize it
      // immediately so we capture negotiated ALPN/cipher/version/client-cert and update
      // metrics/state. This must be done even if the same read later returns an error or
      // EOF — the handshake result is valuable and should be recorded before any
      // connection teardown logic runs.
      // Note: this is a transition action (handshakePending -> done) rather than a
      // normal successful-read action, so it intentionally runs prior to evaluating
      // transport error/EOF handling below.
      if (!pCnx->tlsEstablished && pCnx->transport->handshakeDone()) {
#ifdef AERONET_ENABLE_OPENSSL
        if (_config.tls && dynamic_cast<TlsTransport*>(pCnx->transport.get()) != nullptr) {
          const auto* tlsTr = static_cast<const TlsTransport*>(pCnx->transport.get());
          finalizeTlsHandshake(tlsTr->rawSsl(), cnxFd, _config.tls->logHandshake, pCnx->handshakeStart,
                               pCnx->selectedAlpn, pCnx->negotiatedCipher, pCnx->negotiatedVersion, _tlsMetrics);
        }
#endif
        pCnx->tlsEstablished = true;
      }
      // Close only on fatal transport error or an orderly EOF (bytesRead==0 with no 'want' hint).
      if (want == Transport::Error || (bytesRead == 0 && want == Transport::None)) {
        // If TLS handshake still pending, treat a transport Error as transient and retry later.
        if (want == Transport::Error && pCnx != nullptr && pCnx->transport && !pCnx->transport->handshakeDone()) {
          log::warn("Transient transport error during TLS handshake on fd={}; will retry", cnxFd);
          // Yield and let event loop drive readiness notifications; do not close yet.
          break;
        }
        // Emit richer diagnostics to aid debugging TLS handshake / transport failures.
        log::error("Closing connection fd={} bytesRead={} want={} errno={} ({})", cnxFd, bytesRead,
                   static_cast<int>(want), errno, std::strerror(errno));
#ifdef AERONET_ENABLE_OPENSSL
        if (_tlsCtxHolder) {
          auto* tlsTr = dynamic_cast<TlsTransport*>(pCnx->transport.get());
          if (tlsTr != nullptr) {
            const SSL* ssl = tlsTr->rawSsl();
            if (ssl != nullptr) {
              const char* ver = ::SSL_get_version(ssl);
              const char* cipher = ::SSL_get_cipher_name(ssl);
              log::error("TLS state fd={} ver={} cipher={}", cnxFd, (ver != nullptr) ? ver : "?",
                         (cipher != nullptr) ? cipher : "?");
            }
          }

          TlsTransport::logErrorIfAny();
        }
#endif
        closeConnection(cnxIt);
        pCnx = nullptr;
        break;
      }
      if (want != Transport::None) {
        // Transport indicates we should wait for readability or writability before continuing.
        // Adjust epoll interest if TLS handshake needs write readiness
        if (want == Transport::WriteReady && !pCnx->waitingWritable) {
          if (_eventLoop.mod(cnxFd, EPOLLIN | EPOLLOUT | EPOLLET)) {
            pCnx->waitingWritable = true;
          }
        }
        break;
      }
      if (pCnx->headerStart.time_since_epoch().count() == 0) {
        pCnx->headerStart = std::chrono::steady_clock::now();
      }
      bytesReadThisEvent += static_cast<std::size_t>(bytesRead);
      _telemetry.counterAdd("aeronet.bytes.read", static_cast<uint64_t>(bytesRead));
      if (bytesRead < chunkSize) {
        break;
      }
      if (_config.maxPerEventReadBytes != 0 && bytesReadThisEvent >= _config.maxPerEventReadBytes) {
        break;  // reached fairness cap
      }
    }
    if (pCnx == nullptr) {
      continue;
    }
    bool closeNow = processRequestsOnConnection(cnxIt);
    if (closeNow && pCnx->outBuffer.empty()) {
      closeConnection(cnxIt);
    }
  }
}

HttpServer::ConnectionMapIt HttpServer::closeConnection(ConnectionMapIt cnxIt) {
  const int cfd = cnxIt->first.fd();
  _eventLoop.del(cfd);

  // Best-effort graceful TLS shutdown
#ifdef AERONET_ENABLE_OPENSSL
  if (_config.tls) {
    if (auto* tlsTr = dynamic_cast<TlsTransport*>(cnxIt->second.transport.get())) {
      tlsTr->shutdown();
    }
    // Propagate ALPN mismatch counter from external struct
    _tlsMetrics.alpnStrictMismatches = _tlsMetricsExternal.alpnStrictMismatches;  // capture latest
  }
#endif
  return _connStates.erase(cnxIt);
}

void HttpServer::handleReadableClient(int fd) {
  auto cnxIt = _connStates.find(fd);
  if (cnxIt == _connStates.end()) {
    return;
  }
  ConnectionState& state = cnxIt->second;
  state.lastActivity = std::chrono::steady_clock::now();

  // If there's buffered outbound data, try to flush it FIRST. This handles the case where
  // TLS needs to read before it can continue writing (SSL_ERROR_WANT_READ during SSL_write).
  // We must attempt flush before reading new data to unblock the write operation.
  if (!state.outBuffer.empty()) {
    flushOutbound(cnxIt);
    // Check if connection was closed during flush
    if (state.isAnyCloseRequested()) {
      closeConnection(cnxIt);
      return;
    }
  }

  std::size_t bytesReadThisEvent = 0;
  while (true) {
    Transport want = Transport::None;
    std::size_t chunkSize = _config.bodyReadChunkBytes;
    if (state.headerStart.time_since_epoch().count() != 0 ||
        std::ranges::search(state.buffer, http::DoubleCRLF).empty()) {
      chunkSize = _config.initialReadChunkBytes;
    }
    if (_config.maxPerEventReadBytes != 0) {
      std::size_t remainingBudget =
          (_config.maxPerEventReadBytes > bytesReadThisEvent) ? (_config.maxPerEventReadBytes - bytesReadThisEvent) : 0;
      if (remainingBudget == 0) {
        break;  // fairness budget exhausted
      }
      chunkSize = std::min(chunkSize, remainingBudget);
    }
    const std::size_t count = state.transportRead(chunkSize, want);
    if (!state.tlsEstablished && state.transport->handshakeDone()) {
#ifdef AERONET_ENABLE_OPENSSL
      if (_config.tls && dynamic_cast<TlsTransport*>(state.transport.get()) != nullptr) {
        const auto* tlsTr = static_cast<const TlsTransport*>(state.transport.get());
        finalizeTlsHandshake(tlsTr->rawSsl(), fd, _config.tls->logHandshake, state.handshakeStart, state.selectedAlpn,
                             state.negotiatedCipher, state.negotiatedVersion, _tlsMetrics);
      }
#endif
      state.tlsEstablished = true;
    }
    if (want != Transport::None) {
      // Non-fatal: transport needs the socket to be readable or writable before proceeding.
      if (want == Transport::WriteReady && !state.waitingWritable) {
        if (_eventLoop.mod(fd, EPOLLIN | EPOLLOUT | EPOLLET)) {
          state.waitingWritable = true;
        }
      }
      break;
    }
    if (count == 0) {
      state.requestImmediateClose();
      break;
    }
    if (count > 0 && state.headerStart.time_since_epoch().count() == 0) {
      state.headerStart = std::chrono::steady_clock::now();
    }
    bytesReadThisEvent += static_cast<std::size_t>(count);
    if (_config.maxPerEventReadBytes != 0 && bytesReadThisEvent >= _config.maxPerEventReadBytes) {
      // Reached per-event fairness cap; parse what we have then yield.
      if (processRequestsOnConnection(cnxIt)) {
        break;
      }
      break;
    }
    if (state.buffer.size() > _config.maxHeaderBytes + _config.maxBodyBytes) {
      state.requestImmediateClose();
      break;
    }
    if (processRequestsOnConnection(cnxIt)) {
      break;
    }
    // Header read timeout enforcement: if headers of current pending request are not complete yet
    // (heuristic: no full request parsed and buffer not empty) and duration exceeded -> close.
    if (_config.headerReadTimeout.count() > 0 && state.headerStart.time_since_epoch().count() != 0) {
      if (std::chrono::steady_clock::now() - state.headerStart > _config.headerReadTimeout) {
        state.requestImmediateClose();
        break;
      }
    }
  }
  // Try to flush again after reading new data, in case TLS needed the read to proceed with write
  if (!state.outBuffer.empty()) {
    flushOutbound(cnxIt);
  }
  if (state.isAnyCloseRequested()) {
    closeConnection(cnxIt);
  }
}

void HttpServer::handleWritableClient(int fd) {
  const auto cnxIt = _connStates.find(fd);
  if (cnxIt == _connStates.end()) {
    log::error("Invalid fd {} received from the event loop", fd);
    return;
  }
  flushOutbound(cnxIt);
  if (cnxIt->second.isAnyCloseRequested()) {
    closeConnection(cnxIt);
  }
}

}  // namespace aeronet
