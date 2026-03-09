#include "aeronet/internal/connection-storage.hpp"

#include <chrono>
#include <cstdint>

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-transport.hpp"
#endif

namespace aeronet::internal {

#ifdef AERONET_ENABLE_OPENSSL
void ConnectionStorage::recycleOrRelease(ConnectionIt cnxIt, uint32_t maxCachedConnections, bool tlsEnabled,
                                         uint32_t& handshakesInFlight) {
#else
void ConnectionStorage::recycleOrRelease(ConnectionIt cnxIt, uint32_t maxCachedConnections) {
#endif
  const uint32_t connectionIdx = ConnectionItToIdx(cnxIt);
  auto* pConnection = _activeConnectionStates[connectionIdx];
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  auto& asyncState = pConnection->asyncState;
  if (asyncState.active || asyncState.handle) {
    asyncState.clear();
  }
#endif

  // Best-effort graceful TLS shutdown
#ifdef AERONET_ENABLE_OPENSSL
  if (tlsEnabled) {
    // If the connection is closed mid-handshake, release admission control slot.
    if (pConnection->tlsHandshakeInFlight) {
      pConnection->tlsHandshakeInFlight = false;
      --handshakesInFlight;
    }

    if (auto* tlsTr = dynamic_cast<TlsTransport*>(pConnection->transport.get())) {
      tlsTr->shutdown();
    }
  }
#endif

  // Move ConnectionState to cache for potential reuse
  if (_cachedConnectionStates.size() < maxCachedConnections) {
    _cachedConnectionStates.push_back(pConnection);
  } else {
    _connectionStatePool.destroyAndRelease(pConnection);
  }

  _activeConnections[connectionIdx].close();
  _activeConnectionStates[connectionIdx] = nullptr;

  --_nbActiveConnections;
}

void ConnectionStorage::sweepCachedConnections(std::chrono::steady_clock::duration timeout) {
  const auto deadline = now - timeout;
  auto it = _cachedConnectionStates.begin();
  for (; it != _cachedConnectionStates.end() && (*it)->lastActivity < deadline; ++it) {
    _connectionStatePool.destroyAndRelease(*it);
  }
  _cachedConnectionStates.erase(_cachedConnectionStates.begin(), it);
}

void ConnectionStorage::shrink_to_fit() {
  if (_activeConnectionStates.empty()) {
    return;
  }
  auto activeIt = _activeConnectionStates.end();
  do {
    if (*--activeIt != nullptr) {
      break;
    }
  } while (activeIt != _activeConnectionStates.begin());

  const auto nbConnectionsToSuppress = _activeConnectionStates.end() - ++activeIt;
  _activeConnectionStates.erase(activeIt, _activeConnectionStates.end());
  _activeConnections.erase(_activeConnections.end() - nbConnectionsToSuppress, _activeConnections.end());

  if (_activeConnections.size() > 128UL && _activeConnections.capacity() > _activeConnections.size() * 4UL) {
    _activeConnections.shrink_to_fit();
    _activeConnectionStates.shrink_to_fit();
    _cachedConnectionStates.shrink_to_fit();
  }
}
}  // namespace aeronet::internal