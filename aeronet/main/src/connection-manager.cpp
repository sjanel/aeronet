#include <sys/types.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
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
        !st.tlsEstablished && st.transport->handshakePending()) {
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
      bool wantR = false;
      bool wantW = false;
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
      ssize_t bytesRead = pCnx->transportRead(chunkSize, wantR, wantW);
      // Check for handshake completion
      if (!pCnx->tlsEstablished && !pCnx->transport->handshakePending()) {
#ifdef AERONET_ENABLE_OPENSSL
        if (_config.tls && dynamic_cast<TlsTransport*>(pCnx->transport.get()) != nullptr) {
          auto* tlsTr = static_cast<TlsTransport*>(pCnx->transport.get());
          finalizeTlsHandshake(tlsTr->rawSsl(), cnxFd, _config.tls->logHandshake, pCnx->handshakeStart,
                               pCnx->selectedAlpn, pCnx->negotiatedCipher, pCnx->negotiatedVersion, _tlsMetrics);
        }
#endif
        pCnx->tlsEstablished = true;
      }
      if (bytesRead > 0) {
        if (pCnx->headerStart.time_since_epoch().count() == 0) {
          pCnx->headerStart = std::chrono::steady_clock::now();
        }
        bytesReadThisEvent += static_cast<std::size_t>(bytesRead);
        if (std::cmp_less(bytesRead, chunkSize)) {
          break;
        }
        if (_config.maxPerEventReadBytes != 0 && bytesReadThisEvent >= _config.maxPerEventReadBytes) {
          break;  // reached fairness cap
        }
        continue;
      }
      if (bytesRead == 0) {
        closeConnection(cnxIt);
        pCnx = nullptr;
        break;
      }
      if (bytesRead == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        // Adjust epoll interest if TLS handshake needs write readiness
        pCnx->tlsWantRead = wantR;
        pCnx->tlsWantWrite = wantW;
        if (wantW && !pCnx->waitingWritable) {
          if (_eventLoop.mod(cnxFd, EPOLLIN | EPOLLOUT | EPOLLET)) {
            pCnx->waitingWritable = true;
          }
        }
        break;
      }
      closeConnection(cnxIt);
      pCnx = nullptr;
      break;
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
  std::size_t bytesReadThisEvent = 0;
  while (true) {
    bool wantR = false;
    bool wantW = false;
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
    auto count = state.transportRead(chunkSize, wantR, wantW);
    if (!state.tlsEstablished && !state.transport->handshakePending()) {
#ifdef AERONET_ENABLE_OPENSSL
      if (_config.tls && dynamic_cast<TlsTransport*>(state.transport.get()) != nullptr) {
        auto* tlsTr = static_cast<TlsTransport*>(state.transport.get());
        finalizeTlsHandshake(tlsTr->rawSsl(), fd, _config.tls->logHandshake, state.handshakeStart, state.selectedAlpn,
                             state.negotiatedCipher, state.negotiatedVersion, _tlsMetrics);
      }
#endif
      state.tlsEstablished = true;
    }
    if (count < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        state.tlsWantRead = wantR;
        state.tlsWantWrite = wantW;
        if (wantW && !state.waitingWritable) {
          if (_eventLoop.mod(fd, EPOLLIN | EPOLLOUT | EPOLLET)) {
            state.waitingWritable = true;
          }
        }
        break;
      }
      log::error("read failed: {}", std::strerror(errno));
      state.requestImmediateClose();
      break;
    }
    if (count == 0) {
      state.requestImmediateClose();
      break;
    }
    if (count > 0 && state.headerStart.time_since_epoch().count() == 0) {
      state.headerStart = std::chrono::steady_clock::now();
    }
    if (count > 0) {
      bytesReadThisEvent += static_cast<std::size_t>(count);
      if (_config.maxPerEventReadBytes != 0 && bytesReadThisEvent >= _config.maxPerEventReadBytes) {
        // Reached per-event fairness cap; parse what we have then yield.
        if (processRequestsOnConnection(cnxIt)) {
          break;
        }
        break;
      }
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
}

}  // namespace aeronet
