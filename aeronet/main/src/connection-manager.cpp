#include <asm-generic/socket.h>
#include <netinet/in.h>  //NOLINT(misc-include-cleaner) used by socket options
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/transport.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/types.h>

#include "aeronet/tls-context.hpp"
#include "aeronet/tls-handshake.hpp"
#include "aeronet/tls-raii.hpp"
#include "aeronet/tls-transport.hpp"  // from tls module include directory
#endif

#include "aeronet/event-loop.hpp"
#include "aeronet/log.hpp"

namespace aeronet {

void HttpServer::sweepIdleConnections() {
  // Periodic maintenance of live connections: applies keep-alive timeout (if enabled) and
  // header read timeout (always, regardless of keep-alive enablement). The header read timeout
  // needs a periodic check because a client might send a partial request line then stall; no
  // further EPOLLIN events will arrive to trigger enforcement in handleReadableClient().
  auto now = std::chrono::steady_clock::now();
  for (auto cnxIt = _activeConnectionsMap.begin(); cnxIt != _activeConnectionsMap.end();) {
    ConnectionState& state = *cnxIt->second;

    // Close immediately if requested
    if (state.isImmediateCloseRequested()) {
      cnxIt = closeConnection(cnxIt);
      continue;
    }

    // For DrainThenClose mode, only close after buffers and file payload are fully drained
    if (state.canCloseConnectionForDrain()) {
      cnxIt = closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_drain");
      continue;
    }

    // Keep-alive inactivity enforcement only if enabled.
    // Don't close if there's an active file send - those can block waiting for socket to be writable.
    if (_config.enableKeepAlive && !state.isSendingFile() && now > state.lastActivity + _config.keepAliveTimeout) {
      cnxIt = closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_keep_alive");
      continue;
    }
    // Header read timeout: active if headerStart set and duration exceeded and no full request parsed yet.
    if (_config.headerReadTimeout.count() > 0 && state.headerStartTp.time_since_epoch().count() != 0 &&
        now > state.headerStartTp + _config.headerReadTimeout) {
      emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, false, {});
      cnxIt = closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_header_read_timeout");
      continue;
    }
    // Body read timeout: triggered when the handler is waiting for missing body bytes.
    if (_config.bodyReadTimeout.count() > 0 && state.waitingForBody &&
        state.bodyLastActivity.time_since_epoch().count() != 0 &&
        now > state.bodyLastActivity + _config.bodyReadTimeout) {
      emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, false, {});
      cnxIt = closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_body_read_timeout");
      continue;
    }
    // TLS handshake timeout (if enabled). Applies only while handshake pending.
#ifdef AERONET_ENABLE_OPENSSL
    if (_config.tls.handshakeTimeout.count() > 0 && _config.tls.enabled &&
        state.handshakeStart.time_since_epoch().count() != 0 && !state.tlsEstablished &&
        !state.transport->handshakeDone()) {
      if (now > state.handshakeStart + _config.tls.handshakeTimeout) {
        cnxIt = closeConnection(cnxIt);
        _telemetry.counterAdd("aeronet.connections.closed_for_handshake_timeout");
        continue;
      }
    }
#endif
    ++cnxIt;
  }

  // Clean up cached closed connections older than timeout
  // The connections are ordered by increasing lastActivity time, so we can stop at the first
  // one that is still within the timeout.
  const auto deadline = std::chrono::steady_clock::now() - _config.cachedConnectionsTimeout;
  const auto cutOffIt = std::ranges::partition_point(
      _cachedConnections, [deadline](const auto& ptr) { return ptr->lastActivity < deadline; });
  _cachedConnections.erase(_cachedConnections.begin(), cutOffIt);
}

void HttpServer::acceptNewConnections() {
  while (true) {
    Connection cnx(_listenSocket);
    if (!cnx) {
      // no more waiting connections
      break;
    }
    int cnxFd = cnx.fd();
    if (_config.tcpNoDelay) {
      static constexpr int enable = 1;
      if (::setsockopt(cnxFd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) != 0) {
        auto err = errno;
        log::error("setsockopt(TCP_NODELAY) failed for fd # {} err={} ({})", cnxFd, err, std::strerror(err));
        _telemetry.counterAdd("aeronet.connections.errors.tcp_nodelay_failed", 1UL);
      }
    }
    if (!_eventLoop.add(EventLoop::EventFd{cnxFd, EventIn | EventEt})) {
      auto savedErr = errno;
      log::error("EventLoop add client failed fd # {} err={}: {}", cnxFd, savedErr, std::strerror(savedErr));
      _telemetry.counterAdd("aeronet.connections.errors.add_event_failed", 1UL);
      continue;
    }

    auto [cnxIt, inserted] = _activeConnectionsMap.emplace(std::move(cnx), getNewConnectionState());
    if (!inserted) {
      // This should not happen, if it does, it's probably a bug in the library or a very weird usage of HttpServer.
      log::error("Internal error: accepted connection fd # {} already present in connection map", cnxFd);
      // Close the newly accepted connection immediately to avoid fd leak.
      _eventLoop.del(cnxFd);
      _telemetry.counterAdd("aeronet.connections.errors.duplicate_accept", 1UL);

      assert(false);
      continue;
    }

    // Track new connection acceptance
    _telemetry.counterAdd("aeronet.connections.accepted", 1UL);

    ConnectionState& state = *cnxIt->second;
#ifdef AERONET_ENABLE_OPENSSL
    if (_tlsCtxHolder) {
      SSL_CTX* ctx = reinterpret_cast<SSL_CTX*>(_tlsCtxHolder->raw());
      SslPtr sslPtr(SSL_new(ctx), SSL_free);
      if (sslPtr.get() == nullptr) {
        continue;
      }

      if (SSL_set_fd(sslPtr.get(), cnxFd) != 1) {  // associate
        log::error("SSL_set_fd failed for fd # {}", cnxFd);
        continue;
      }
      // Enable partial writes: SSL_write will return after writing some data rather than
      // trying to write everything. This is crucial for non-blocking I/O performance.
      SSL_set_mode(sslPtr.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
      ::SSL_set_accept_state(sslPtr.get());
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
      if (pCnx->headerStartTp.time_since_epoch().count() != 0 ||
          std::ranges::search(pCnx->inBuffer, http::DoubleCRLF).empty()) {
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
      const auto [bytesRead, want] = pCnx->transportRead(chunkSize);
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
        if (_config.tls.enabled && dynamic_cast<TlsTransport*>(pCnx->transport.get()) != nullptr) {
          auto* tlsTr = static_cast<TlsTransport*>(pCnx->transport.get());
          pCnx->tlsInfo =
              FinalizeTlsHandshake(tlsTr->rawSsl(), cnxFd, _config.tls.logHandshake, pCnx->handshakeStart, _tlsMetrics);
#ifdef AERONET_ENABLE_KTLS
          maybeEnableKtlsSend(*pCnx, *tlsTr, cnxFd);
#endif
        }
#endif
        pCnx->tlsEstablished = true;
        if (pCnx->isImmediateCloseRequested()) {
          cnxIt = closeConnection(cnxIt);
          pCnx = nullptr;
          break;
        }
      }
      if (pCnx->waitingForBody && bytesRead > 0) {
        pCnx->bodyLastActivity = std::chrono::steady_clock::now();
      }
      // Close only on fatal transport error or an orderly EOF (bytesRead==0 with no 'want' hint).
      if (want == TransportHint::Error || (bytesRead == 0 && want == TransportHint::None)) {
        // If TLS handshake still pending, treat a transport Error as transient and retry later.
        if (want == TransportHint::Error) {
          if (pCnx != nullptr && pCnx->transport && !pCnx->transport->handshakeDone()) {
            log::debug("Transient transport error during TLS handshake on fd # {}; will retry", cnxFd);
            // Yield and let event loop drive readiness notifications; do not close yet.
            break;
          }  // Emit richer diagnostics to aid debugging TLS handshake / transport failures.
          log::error("Closing connection fd # {} bytesRead={} want={} errno={} ({})", cnxFd, bytesRead,
                     static_cast<int>(want), errno, std::strerror(errno));
#ifdef AERONET_ENABLE_OPENSSL
          if (_tlsCtxHolder) {
            auto* tlsTr = dynamic_cast<TlsTransport*>(pCnx->transport.get());
            if (tlsTr != nullptr) {
              const SSL* ssl = tlsTr->rawSsl();
              if (ssl != nullptr) {
                const char* ver = ::SSL_get_version(ssl);
                const char* cipher = ::SSL_get_cipher_name(ssl);
                log::error("TLS state fd # {} ver={} cipher={}", cnxFd, (ver != nullptr) ? ver : "?",
                           (cipher != nullptr) ? cipher : "?");
              }
              tlsTr->logErrorIfAny();
            }
          }
#endif
        }
        closeConnection(cnxIt);
        pCnx = nullptr;
        break;
      }
      if (want != TransportHint::None) {
        // Transport indicates we should wait for readability or writability before continuing.
        // Adjust epoll interest if TLS handshake needs write readiness
        if (want == TransportHint::WriteReady && !pCnx->waitingWritable) {
          pCnx->waitingWritable = _eventLoop.mod(EventLoop::EventFd{cnxFd, EventIn | EventOut | EventEt});
        }
        break;
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
    const bool closeNow = processRequestsOnConnection(cnxIt);
    if (closeNow && pCnx->outBuffer.empty() && pCnx->tunnelOrFileBuffer.empty() && !pCnx->isSendingFile()) {
      closeConnection(cnxIt);
    }
  }
}

HttpServer::ConnectionMapIt HttpServer::closeConnection(ConnectionMapIt cnxIt) {
  const int cfd = cnxIt->first.fd();

  _eventLoop.del(cfd);

  auto& asyncState = cnxIt->second->asyncState;
  if (asyncState.active || asyncState.handle) {
    asyncState.clear();
  }

  // Best-effort graceful TLS shutdown
#ifdef AERONET_ENABLE_OPENSSL
  if (_config.tls.enabled) {
    if (auto* tlsTr = dynamic_cast<TlsTransport*>(cnxIt->second->transport.get())) {
      tlsTr->shutdown();
    }
    // Propagate ALPN mismatch counter from external struct
    _tlsMetrics.alpnStrictMismatches = _tlsMetricsExternal.alpnStrictMismatches;  // capture latest
  }
#endif

  // Move ConnectionState to cache for potential reuse
  if (_config.cachedConnectionsTimeout.count() > 0) {
    auto statePtr = std::move(cnxIt->second);
    statePtr->lastActivity = std::chrono::steady_clock::now();

    _cachedConnections.push_back(std::move(statePtr));
  }

  return _activeConnectionsMap.erase(cnxIt);
}

void HttpServer::handleReadableClient(int fd) {
  auto cnxIt = _activeConnectionsMap.find(fd);
  if (cnxIt == _activeConnectionsMap.end()) {
    return;
  }
  ConnectionState& state = *cnxIt->second;
  state.lastActivity = std::chrono::steady_clock::now();

  // If there's buffered outbound data, try to flush it FIRST. This handles the case where
  // TLS needs to read before it can continue writing (SSL_ERROR_WANT_READ during SSL_write).
  // We must attempt flush before reading new data to unblock the write operation.
  if (!state.outBuffer.empty()) {
    flushOutbound(cnxIt);
    // Check if connection was closed during flush
    if (state.canCloseImmediately()) {
      closeConnection(cnxIt);
      return;
    }
  }

  // If in tunneling mode, read raw bytes and forward to peer
  if (state.isTunneling()) {
    handleInTunneling(cnxIt);
    return;
  }

  std::size_t bytesReadThisEvent = 0;
  while (true) {
    std::size_t chunkSize = _config.bodyReadChunkBytes;
    if (state.headerStartTp.time_since_epoch().count() != 0 ||
        std::ranges::search(state.inBuffer, http::DoubleCRLF).empty()) {
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
    const auto [count, want] = state.transportRead(chunkSize);
    if (!state.tlsEstablished && state.transport->handshakeDone()) {
#ifdef AERONET_ENABLE_OPENSSL
      if (_config.tls.enabled && dynamic_cast<TlsTransport*>(state.transport.get()) != nullptr) {
        auto* tlsTr = static_cast<TlsTransport*>(state.transport.get());
        state.tlsInfo =
            FinalizeTlsHandshake(tlsTr->rawSsl(), fd, _config.tls.logHandshake, state.handshakeStart, _tlsMetrics);
#ifdef AERONET_ENABLE_KTLS
        maybeEnableKtlsSend(state, *tlsTr, fd);
#endif
      }
#endif
      state.tlsEstablished = true;
#if defined(AERONET_ENABLE_OPENSSL) && defined(AERONET_ENABLE_KTLS)
      if (state.isImmediateCloseRequested()) {
        closeConnection(cnxIt);
        return;
      }
#endif
    }
    if (state.waitingForBody && count > 0) {
      state.bodyLastActivity = std::chrono::steady_clock::now();
    }
    if (want != TransportHint::None) {
      // Non-fatal: transport needs the socket to be readable or writable before proceeding.
      if (want == TransportHint::WriteReady && !state.waitingWritable) {
        state.waitingWritable = _eventLoop.mod(EventLoop::EventFd{fd, EventIn | EventOut | EventEt});
      }
      break;
    }
    if (count == 0) {
      state.requestImmediateClose();
      break;
    }
    bytesReadThisEvent += static_cast<std::size_t>(count);
    if (_config.maxPerEventReadBytes != 0 && bytesReadThisEvent >= _config.maxPerEventReadBytes) {
      // Reached per-event fairness cap; parse what we have then yield.
      if (processConnectionInput(cnxIt)) {
        break;
      }
      break;
    }
    if (state.inBuffer.size() > _config.maxHeaderBytes + _config.maxBodyBytes) {
      state.requestImmediateClose();
      break;
    }
    if (processConnectionInput(cnxIt)) {
      break;
    }
    // Header read timeout enforcement: if headers of current pending request are not complete yet
    // (heuristic: no full request parsed and buffer not empty) and duration exceeded -> close.
    if (_config.headerReadTimeout.count() > 0 && state.headerStartTp.time_since_epoch().count() != 0) {
      if (std::chrono::steady_clock::now() - state.headerStartTp > _config.headerReadTimeout) {
        emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, false, {});
        state.requestImmediateClose();
        break;
      }
    }
  }
  // Try to flush again after reading new data, in case TLS needed the read to proceed with write
  if (!state.outBuffer.empty()) {
    flushOutbound(cnxIt);
  }
  if (state.canCloseImmediately()) {
    closeConnection(cnxIt);
  }
}

void HttpServer::handleWritableClient(int fd) {
  const auto cnxIt = _activeConnectionsMap.find(fd);
  if (cnxIt == _activeConnectionsMap.end()) {
    log::error("Received an invalid fd # {} from the event loop (or already removed?)", fd);
    return;
  }
  ConnectionState& state = *cnxIt->second;
  // If this connection was created for an upstream non-blocking connect, and connect is pending,
  // check SO_ERROR to determine whether connect completed successfully or failed.
  if (state.connectPending) {
    int soerr = 0;
    socklen_t len = sizeof(soerr);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0) {
      state.connectPending = false;
      if (soerr != 0) {
        // Upstream connect failed. Attempt to notify the client side (peerFd) with 502 and close both.
        const auto peerIt = _activeConnectionsMap.find(state.peerFd);
        if (peerIt != _activeConnectionsMap.end()) {
          emitSimpleError(peerIt, http::StatusCodeBadGateway, true, "Upstream connect failed");
        } else {
          log::error("Unable to notify client of upstream connect failure: peer fd # {} not found", state.peerFd);
        }
        closeConnection(cnxIt);
        return;
      }
      // otherwise connect succeeded; continue to normal writable handling
    }
  }
  // If tunneling, flush tunnelOutBuffer first
  if (state.isTunneling() && !state.tunnelOrFileBuffer.empty()) {
    const auto [written, want] = state.transportWrite(state.tunnelOrFileBuffer);
    if (want == TransportHint::Error) {
      // Fatal error writing tunnel data: close this connection
      closeConnection(cnxIt);
      return;
    }
    if (written > 0) {
      state.tunnelOrFileBuffer.erase_front(written);
    }
    // If still has data, keep EPOLLOUT registered
    if (!state.tunnelOrFileBuffer.empty()) {
      return;
    }
    // Tunnel buffer drained: fall through to normal flushOutbound handling
  }
  flushOutbound(cnxIt);
  if (state.canCloseImmediately()) {
    closeConnection(cnxIt);
  }
}

void HttpServer::handleInTunneling(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
  std::size_t bytesReadThisEvent = 0;
  while (true) {
    std::size_t chunk = _config.bodyReadChunkBytes;
    const auto [bytesRead, want] = state.transportRead(chunk);
    if (want == TransportHint::Error || (bytesRead == 0 && want == TransportHint::None)) {
      closeConnection(cnxIt);
      return;
    }
    if (want != TransportHint::None) {
      break;
    }
    bytesReadThisEvent += bytesRead;
    if (bytesRead < chunk) {
      break;
    }
    if (_config.maxPerEventReadBytes != 0 && bytesReadThisEvent >= _config.maxPerEventReadBytes) {
      break;
    }
  }
  if (state.inBuffer.empty()) {
    return;
  }
  auto peerIt = _activeConnectionsMap.find(state.peerFd);
  if (peerIt == _activeConnectionsMap.end()) {
    closeConnection(cnxIt);
    return;
  }
  ConnectionState& peer = *peerIt->second;
  const auto [written, want] = peer.transportWrite(state.inBuffer);
  if (want == TransportHint::Error) {
    // Fatal transport error while forwarding to peer: close both sides.
    closeConnection(peerIt);
    closeConnection(cnxIt);
    return;
  }
  if (written > 0) {
    state.inBuffer.erase_front(written);
  }
  if (!state.inBuffer.empty()) {
    if (peer.tunnelOrFileBuffer.empty()) {
      state.inBuffer.swap(peer.tunnelOrFileBuffer);
    } else {
      peer.tunnelOrFileBuffer.append(state.inBuffer);
      state.inBuffer.clear();
    }

    if (!peer.waitingWritable) {
      enableWritableInterest(peerIt, "enable peer writable tunnel");
    }
  }
}

}  // namespace aeronet
