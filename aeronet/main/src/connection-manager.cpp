#include <sys/types.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

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
#include "sys-utils.hpp"

namespace aeronet {

namespace {
inline constexpr std::size_t kChunkSize = 4096;
}

void HttpServer::sweepIdleConnections() {
  // Periodic maintenance of live connections: applies keep-alive timeout (if enabled) and
  // header read timeout (always, regardless of keep-alive enablement). The header read timeout
  // needs a periodic check because a client might send a partial request line then stall; no
  // further EPOLLIN events will arrive to trigger enforcement in handleReadableClient().
  auto now = std::chrono::steady_clock::now();
  for (auto it = _connStates.begin(); it != _connStates.end();) {
    bool closeThis = false;
    ConnectionState& st = it->second;
    // Keep-alive inactivity enforcement only if enabled.
    if (_config.enableKeepAlive) {
      if (st.shouldClose || (now - st.lastActivity) > _config.keepAliveTimeout) {
        closeThis = true;
      }
    } else if (st.shouldClose) {
      closeThis = true;
    }
    // Header read timeout: active if headerStart set and duration exceeded and no full request parsed yet.
    if (!closeThis && _config.headerReadTimeout.count() > 0 && st.headerStart.time_since_epoch().count() != 0) {
      if (now - st.headerStart > _config.headerReadTimeout) {
        closeThis = true;
      }
    }
    // TLS handshake timeout (if enabled). Applies only while handshake pending.
#ifdef AERONET_ENABLE_OPENSSL
    if (!closeThis && _config.tlsHandshakeTimeout.count() > 0 && _config.tls && st.transport &&
        st.handshakeStart.time_since_epoch().count() != 0 && !st.tlsEstablished && st.transport->handshakePending()) {
      if (now - st.handshakeStart > _config.tlsHandshakeTimeout) {
        closeThis = true;
      }
    }
#endif
    if (closeThis) {
      int connFd = it->first.fd();
      ++it;
      closeConnectionFd(connFd);
      continue;
    }
    ++it;
  }
}

void HttpServer::acceptNewConnections() {
  while (true) {
    Connection cnx(_listenSocket);
    if (!cnx.isOpened()) {
      break;
    }
    if (setNonBlocking(cnx.fd()) < 0) {
      auto savedErr = errno;
      log::error("setNonBlocking failed fd={} err={}: {}", cnx.fd(), savedErr, std::strerror(savedErr));
      continue;
    }
    if (!_loop.add(cnx.fd(), EPOLLIN | EPOLLET)) {
      auto savedErr = errno;
      log::error("EventLoop add client failed fd={} err={}: {}", cnx.fd(), savedErr, std::strerror(savedErr));
      continue;
    }
    auto cnxFd = cnx.fd();
    auto [itState, inserted] = _connStates.emplace(std::move(cnx), ConnectionState{});
    ConnectionState& stRef = itState->second;
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
      stRef.transport = std::make_unique<TlsTransport>(std::move(sslPtr));
      stRef.handshakeStart = std::chrono::steady_clock::now();
    } else {
      stRef.transport = std::make_unique<PlainTransport>(cnxFd);
    }
#else
    stRef.transport = std::make_unique<PlainTransport>(cnxFd);
#endif
    ConnectionState* pst = &itState->second;
    while (true) {
      bool wantR = false;
      bool wantW = false;
      ssize_t bytesRead = transportRead(cnxFd, *pst, kChunkSize, wantR, wantW);
      // Check for handshake completion
      if (!pst->tlsEstablished && pst->transport && !pst->transport->handshakePending()) {
#ifdef AERONET_ENABLE_OPENSSL
        if (_config.tls && dynamic_cast<TlsTransport*>(pst->transport.get()) != nullptr) {
          auto* tlsTr = static_cast<TlsTransport*>(pst->transport.get());
          finalizeTlsHandshake(tlsTr->rawSsl(), cnxFd, _config.tls->logHandshake, pst->handshakeStart,
                               pst->selectedAlpn, pst->negotiatedCipher, pst->negotiatedVersion, _tlsMetrics);
        }
#endif
        pst->tlsEstablished = true;
      }
      if (bytesRead > 0) {
        if (pst->headerStart.time_since_epoch().count() == 0) {
          pst->headerStart = std::chrono::steady_clock::now();
        }
        if (std::cmp_less(bytesRead, kChunkSize)) {
          break;
        }
        continue;
      }
      if (bytesRead == 0) {
        closeConnectionFd(cnxFd);
        pst = nullptr;
        break;
      }
      if (bytesRead == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        // Adjust epoll interest if TLS handshake needs write readiness
        pst->tlsWantRead = wantR;
        pst->tlsWantWrite = wantW;
        if (wantW && !pst->waitingWritable) {
          if (_loop.mod(cnxFd, EPOLLIN | EPOLLOUT | EPOLLET)) {
            pst->waitingWritable = true;
          }
        }
        break;
      }
      closeConnectionFd(cnxFd);
      pst = nullptr;
      break;
    }
    if (pst == nullptr) {
      continue;
    }
    bool closeNow = processRequestsOnConnection(cnxFd, *pst);
    if (closeNow && pst->outBuffer.empty()) {
      closeConnectionFd(cnxFd);
      pst = nullptr;
    }
  }
}

void HttpServer::closeConnectionFd(int cfd) {
  _loop.del(cfd);

  auto it = _connStates.find(cfd);
  if (it != _connStates.end()) {
    // Best-effort graceful TLS shutdown
#ifdef AERONET_ENABLE_OPENSSL
    if (_config.tls) {
      if (auto* tlsTr = dynamic_cast<TlsTransport*>(it->second.transport.get())) {
        tlsTr->shutdown();
      }
      // Propagate ALPN mismatch counter from external struct
      _tlsMetrics.alpnStrictMismatches = _tlsMetricsExternal.alpnStrictMismatches;  // capture latest
    }
#endif
    it->second.transport.reset();
    _connStates.erase(it);
  }
}

void HttpServer::handleReadableClient(int fd) {
  auto itState = _connStates.find(fd);
  if (itState == _connStates.end()) {
    return;
  }
  ConnectionState& state = itState->second;
  state.lastActivity = std::chrono::steady_clock::now();
  bool closeConnection = false;
  while (true) {
    bool wantR = false;
    bool wantW = false;
    ssize_t count = transportRead(fd, state, kChunkSize, wantR, wantW);
    if (!state.tlsEstablished && state.transport && !state.transport->handshakePending()) {
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
          if (_loop.mod(fd, EPOLLIN | EPOLLOUT | EPOLLET)) {
            state.waitingWritable = true;
          }
        }
        break;
      }
      log::error("read failed: {}", std::strerror(errno));
      closeConnection = true;
      break;
    }
    if (count == 0) {
      closeConnection = true;
      break;
    }
    if (count > 0 && state.headerStart.time_since_epoch().count() == 0) {
      state.headerStart = std::chrono::steady_clock::now();
    }
    if (state.buffer.size() > _config.maxHeaderBytes + _config.maxBodyBytes) {
      closeConnection = true;
      break;
    }
    if (processRequestsOnConnection(fd, state)) {
      closeConnection = true;
      break;
    }
    // Header read timeout enforcement: if headers of current pending request are not complete yet
    // (heuristic: no full request parsed and buffer not empty) and duration exceeded -> close.
    if (_config.headerReadTimeout.count() > 0 && state.headerStart.time_since_epoch().count() != 0) {
      const auto now = std::chrono::steady_clock::now();
      if (now - state.headerStart > _config.headerReadTimeout) {
        closeConnection = true;
        break;
      }
    }
  }
  if (closeConnection) {
    closeConnectionFd(fd);
  }
}

void HttpServer::handleWritableClient(int fd) {
  auto it = _connStates.find(fd);
  if (it == _connStates.end()) {
    return;
  }
  flushOutbound(fd, it->second);
}

}  // namespace aeronet
