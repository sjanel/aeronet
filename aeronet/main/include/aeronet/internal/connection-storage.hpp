#pragma once

#include <cassert>
#include <cstdint>

#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

class ConnectionStorage {
 public:
  //  - The server and request layer rely on a stable ConnectionState address:
  // TODO: change that, we can store vector + idx instead of raw pointer.
  //      * HttpRequest stores `ConnectionState* _ownerState` and uses it for async/body coordination
  //        (e.g. HttpRequest::markAwaitingBody()) and body access bridges.
  //      * The event loop code often keeps `ConnectionState&` / `ConnectionState*` across helper calls
  //        that may emplace new connections (see processSpecialMethods comment in the .cpp).
  //    If ConnectionState were stored by value in the hash table, these pointers/references could dangle.

  using ConnectionIt = vector<Connection>::iterator;

 private:
  [[nodiscard]] static uint32_t ConnectionItToIdx(ConnectionIt it) { return static_cast<uint32_t>(it->fd() - 1); }

 public:
#ifdef AERONET_ENABLE_OPENSSL
  void recycleOrRelease(ConnectionIt cnxIt, uint32_t maxCachedConnections, bool tlsEnabled,
                        uint32_t& handshakesInFlight);
#else
  void recycleOrRelease(ConnectionIt cnxIt, uint32_t maxCachedConnections);
#endif

  ConnectionIt emplace(Connection&& cnx) {
    const NativeHandle fd = cnx.fd();
    assert(fd != 0);
    const auto connectionIdx = static_cast<vector<Connection>::size_type>(fd - 1);

    while (_activeConnections.size() < connectionIdx + 1U) {
      _activeConnections.emplace_back();
      _activeConnectionStates.emplace_back();
    }

    _activeConnections[connectionIdx] = std::move(cnx);
    _activeConnectionStates[connectionIdx] = getNewConnectionState();
    ++_nbActiveConnections;

    return _activeConnections.begin() + connectionIdx;
  }

  void sweepCachedConnections(std::chrono::steady_clock::duration timeout);

  void shrink_to_fit();

  [[nodiscard]] std::size_t nbCachedConnections() const noexcept { return _cachedConnectionStates.size(); }

  ConnectionIt begin() { return _activeConnections.begin(); }
  ConnectionIt end() { return _activeConnections.end(); }

  ConnectionIt iterator(NativeHandle fd) { return _activeConnections.begin() + (fd - 1); }

  [[nodiscard]] std::size_t size() const { return _nbActiveConnections; }

  ConnectionState& connectionState(ConnectionIt cnxIt) { return *_activeConnectionStates[ConnectionItToIdx(cnxIt)]; }

  ConnectionState* pConnectionState(ConnectionIt cnxIt) { return _activeConnectionStates[ConnectionItToIdx(cnxIt)]; }
  ConnectionState* pConnectionState(NativeHandle fd) { return _activeConnectionStates[static_cast<uint32_t>(fd - 1)]; }

  std::chrono::steady_clock::time_point now;

 private:
  ConnectionState* getNewConnectionState() {
    ConnectionState* statePtr;
    if (_cachedConnectionStates.empty()) {
      statePtr = _connectionStatePool.allocateAndConstruct();
      statePtr->request._ownerState = statePtr;
    } else {
      statePtr = _cachedConnectionStates.back();
      _cachedConnectionStates.pop_back();
      statePtr->reset();
    }
    statePtr->lastActivity = now;
    return statePtr;
  }

  ObjectPool<ConnectionState> _connectionStatePool;

  vector<Connection> _activeConnections;
  vector<ConnectionState*> _activeConnectionStates;
  vector<ConnectionState*> _cachedConnectionStates;  // cache of closed ConnectionState objects for reuse
  std::size_t _nbActiveConnections{};
};

}  // namespace aeronet::internal