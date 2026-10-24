#pragma once

#include <cassert>
#include <chrono>
#include <cstdint>

#include "aeronet/connection-state.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/internal/keep-alive-deadline-queue.hpp"
#include "aeronet/native-handle.hpp"

namespace aeronet::internal {

struct ServerContext;

// The timeouts of the connections of a SingleHttpServer: the deadline queue of the idle keep-alive connections (also
// watching the lingering closes, the WebSocket timeouts and the zerocopy sends in flight), the header read, body read
// and per-route request timers, and the periodic sweep of the connections that need a closer look (missed writable
// edges, timeouts, HTTP/2 stream deadlines, drain closes).
class ConnectionMaintenance {
 public:
  using ConnectionIt = ConnectionStorage::ConnectionIt;

  explicit ConnectionMaintenance(ServerContext& ctx) noexcept : _pCtx(&ctx) {}

  // Points to the context of the server owning this component, after a move of that context.
  void bind(ServerContext& ctx) noexcept { _pCtx = &ctx; }

  // Registers (or unregisters) the keep-alive deadline of a connection according to its current state.
  void refreshKeepAliveDeadline(ConnectionIt cnxIt);

  // Restart the keep-alive idle window from workEnd when serving the request outlived keepAliveTimeout,
  // so a slow handler cannot get its own response swept away as if the connection had been idle.
  void restartKeepAliveIdleWindow(NativeHandle fd, std::chrono::steady_clock::time_point workEnd);
  void restartKeepAliveIdleWindow(NativeHandle fd) { restartKeepAliveIdleWindow(fd, std::chrono::steady_clock::now()); }

  // Registers the deadlines of all the connections again (the timeouts of the configuration changed).
  void rebuildKeepAliveDeadlines();

  // Arms the deadline of a connection at expiresAt, unless one is armed already.
  void armDeadlineIfUnset(ConnectionState& state, NativeHandle fd, std::chrono::steady_clock::time_point expiresAt);

  // Arms the deadline of a connection at expiresAt, replacing the armed one.
  void armDeadline(ConnectionState& state, NativeHandle fd, std::chrono::steady_clock::time_point expiresAt) {
    _deadlines.upsert(state, fd, expiresAt);
  }

  // The timers below only update the connection and the counts deciding the sweeps (see ConnectionSweepState): inlined,
  // as they run for each request.

  // Starts the header read timer of the next HTTP/1.1 request of the connection (see
  // HttpServerConfig::headerReadTimeout), whose start is also the reference of the relative deadlines of the request.
  void startHeaderReadTimer(ConnectionState& state) noexcept {
    assert(!state.parsingHeaders && state.protocolHandler == nullptr);
    state.headerStartTp = state.lastActivity;
    state.parsingHeaders = true;
    ++_sweepState.headerReadConnections;
  }

  // Stops it: the request head was received, or the connection no longer receives HTTP/1.1 requests.
  void stopHeaderReadTimer(ConnectionState& state) noexcept {
    if (state.parsingHeaders) {
      assert(_sweepState.headerReadConnections > 0U);
      --_sweepState.headerReadConnections;
      state.parsingHeaders = false;
    }
  }

  // Records bytes received at 'now' on the connection: the first bytes of the request following a response start its
  // header read timer (the timer of the first request started at the accept), and bytes of a body being received
  // update its reception progress.
  void trackReceivedBytes(ConnectionState& state, std::chrono::steady_clock::time_point now) noexcept {
    if (state.headerStartTp.time_since_epoch().count() == 0) {
      startHeaderReadTimer(state);
    }
    if (state.waitingForBody) {
      state.bodyLastActivityMs = static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now - state.headerStartTp).count());
    }
  }

  void clearRequestDeadline(ConnectionState& state) noexcept {
    if (state.requestDeadlineMs != ConnectionState::kInactiveRelativeMs) {
      state.requestDeadlineMs = ConnectionState::kInactiveRelativeMs;
      assert(_sweepState.requestDeadlineConnections > 0U);
      --_sweepState.requestDeadlineConnections;
    }
  }

  void trackRequestDeadline(ConnectionState& state, uint32_t deadlineMs) noexcept {
    if (state.requestDeadlineMs == ConnectionState::kInactiveRelativeMs) {
      ++_sweepState.requestDeadlineConnections;
    }
    state.requestDeadlineMs = deadlineMs;
  }

  // Track the reception of the body of the current HTTP/1.1 request for the body read timeout. A received body (or a
  // closed connection: bodyReady true) is no longer awaited, even if the timeout was disabled since.
  void trackBodyReception(ConnectionState& state, bool bodyReady) noexcept {
    // The common case, inlined: a body received at once was never awaited.
    if (!bodyReady || state.waitingForBody) {
      updateBodyReception(state, bodyReady);
    }
  }

#ifdef AERONET_ENABLE_HTTP2
  // Counts a connection switched to HTTP/2, whose stream deadlines are swept.
  void trackHttp2Connection() noexcept { ++_sweepState.http2Connections; }
#endif

  // Forgets all the timers and deadlines of a closing connection.
  void forgetConnectionMaintenance(ConnectionState& state) noexcept;

#ifdef AERONET_ENABLE_WEBSOCKET
  // Arms the deadline of a WebSocket connection when its next timeout check comes before the armed deadline (or
  // removes it when no timeout applies).
  void refreshWebSocketDeadline(NativeHandle fd, ConnectionState& state);
#endif

  // Periodic maintenance of the connections. The keep-alive idle timeout is handled by the deadline queue, so that idle
  // HTTP/1.1 connections do not require an O(n) scan. The full scan is reserved for the states that still need periodic
  // observation: missed writable edges, header / body / request / TLS timeouts, HTTP/2 stream deadlines, and graceful
  // drain closes.
  void sweepIdleConnections();

 private:
  // trackBodyReception() of a body awaited, or still awaited.
  void updateBodyReception(ConnectionState& state, bool bodyReady) noexcept;

  bool closeExpiredKeepAliveConnections();

  [[nodiscard]] bool needsFullConnectionMaintenanceSweep() const noexcept;

  // Closes the connection if its header read, body read, per-route request or TLS handshake timeout elapsed. Returns
  // true if it was closed.
  bool closeIfTimedOut(ConnectionIt cnxIt, std::chrono::steady_clock::time_point now);

#ifdef AERONET_LINUX
  // Handles the expired deadline of a connection whose zerocopy sends are in flight: an idle keep-alive connection is
  // closed once they complete (like copied data the kernel keeps sending after close), and a connection whose sends
  // made no progress during a whole keepAliveTimeout is reset. Returns true if the connection was closed.
  bool watchZerocopySends(ConnectionIt cnxIt);
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
  // Runs the idle and close timeouts of a WebSocket connection whose deadline elapsed. Returns true if it was closed.
  bool checkWebSocketTimeouts(ConnectionIt cnxIt);
#endif

  struct ConnectionSweepState {
    uint32_t requestDeadlineConnections{0};
#ifdef AERONET_ENABLE_HTTP2
    uint32_t http2Connections{0};
#endif
    // The connections receiving a request head (ConnectionState::parsingHeaders) and those awaiting body bytes
    // (ConnectionState::waitingForBody). They follow these flags whatever headerReadTimeout and bodyReadTimeout, which
    // a configuration update can change while requests are received: only the sweep decision depends on them.
    uint32_t headerReadConnections{0};
    uint32_t bodyReadConnections{0};
  };

  ServerContext* _pCtx;
  KeepAliveDeadlineQueue _deadlines;
  ConnectionSweepState _sweepState;
};

}  // namespace aeronet::internal
