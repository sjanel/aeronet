#pragma once

#include <cstdint>
#include <functional>

#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-transport.hpp"
#endif

namespace aeronet::internal {

class ConnectionStorage {
 public:
  //  - The server and request layer rely on a stable ConnectionState address:
  //      * HttpRequest stores `ConnectionState* _ownerState` and uses it for async/body coordination
  //        (e.g. HttpRequest::markAwaitingBody()) and body access bridges.
  //      * The event loop code often keeps `ConnectionState&` / `ConnectionState*` across helper calls
  //        that may emplace new connections (see processSpecialMethods comment in the .cpp).
  //    If ConnectionState were stored by value in the hash table, these pointers/references could dangle.
  using ConnectionMap = flat_hash_map<Connection, ConnectionState*, std::hash<int>, std::equal_to<>>;

  using ConnectionMapIt = ConnectionMap::iterator;

#ifdef AERONET_ENABLE_OPENSSL
  ConnectionMapIt recycleOrRelease(uint32_t maxCachedConnections, bool tlsEnabled, ConnectionMapIt cnxIt,
                                   uint32_t& handshakesInFlight) {
#else
  ConnectionMapIt recycleOrRelease(uint32_t maxCachedConnections, ConnectionMapIt cnxIt) {
#endif
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    auto& asyncState = cnxIt->second->asyncState;
    if (asyncState.active || asyncState.handle) {
      asyncState.clear();
    }
#endif

    // Best-effort graceful TLS shutdown
#ifdef AERONET_ENABLE_OPENSSL
    if (tlsEnabled) {
      // If the connection is closed mid-handshake, release admission control slot.
      if (cnxIt->second->tlsHandshakeInFlight) {
        cnxIt->second->tlsHandshakeInFlight = false;
        --handshakesInFlight;
      }

      if (auto* tlsTr = dynamic_cast<TlsTransport*>(cnxIt->second->transport.get())) {
        tlsTr->shutdown();
      }
    }
#endif

    // Move ConnectionState to cache for potential reuse
    if (_cachedConnections.size() < maxCachedConnections) {
      _cachedConnections.push_back(cnxIt->second);
    } else {
      _connectionStatePool.destroyAndRelease(cnxIt->second);
    }

    return active.erase(cnxIt);
  }

  auto emplace(Connection&& cnx) { return active.emplace(std::move(cnx), getNewConnectionState()); }

  void sweepCachedConnections(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration timeout) {
    const auto deadline = now - timeout;
    auto it = _cachedConnections.begin();
    for (; it != _cachedConnections.end() && (*it)->lastActivity < deadline; ++it) {
      _connectionStatePool.destroyAndRelease(*it);
    }
    _cachedConnections.erase(_cachedConnections.begin(), it);
  }

  [[nodiscard]] std::size_t nbCachedConnections() const noexcept { return _cachedConnections.size(); }

  ConnectionMap active;

 private:
  ConnectionState* getNewConnectionState() {
    if (!_cachedConnections.empty()) {
      // Reuse a cached ConnectionState object
      ConnectionState* statePtr = _cachedConnections.back();
      _cachedConnections.pop_back();
      statePtr->reset();
      return statePtr;
    }
    auto* pObj = _connectionStatePool.allocateAndConstruct();
    pObj->request._ownerState = pObj;
    return pObj;
  }

  ObjectPool<ConnectionState> _connectionStatePool;
  vector<ConnectionState*> _cachedConnections;  // cache of closed ConnectionState objects for reuse
};

}  // namespace aeronet::internal