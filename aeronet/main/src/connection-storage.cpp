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
#ifdef AERONET_WINDOWS
  auto* pConnectionState = cnxIt._it->second;
#else
  const auto connectionIdx = ConnectionItToIdx(cnxIt);
  auto* pConnectionState = _activeConnectionStates[connectionIdx];
#endif
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  auto& asyncState = pConnectionState->asyncState;
  if (asyncState.active || asyncState.handle) {
    asyncState.clear();
  }
#endif

  // Best-effort graceful TLS shutdown
#ifdef AERONET_ENABLE_OPENSSL
  if (tlsEnabled) {
    // If the connection is closed mid-handshake, release admission control slot.
    if (pConnectionState->tlsHandshakeInFlight) {
      pConnectionState->tlsHandshakeInFlight = false;
      --handshakesInFlight;
    }

    if (auto* tlsTr = dynamic_cast<TlsTransport*>(pConnectionState->transport.get())) {
      tlsTr->shutdown();
    }
  }
#endif

  // Move ConnectionState to cache for potential reuse
  if (_cachedConnectionStates.size() < maxCachedConnections) {
    _cachedConnectionStates.push_back(pConnectionState);
  } else {
    _connectionStatePool.destroyAndRelease(pConnectionState);
  }

#ifdef AERONET_WINDOWS
  cnxIt._it->first.close();
  _activeConnections.erase(cnxIt._it);
#else
  _activeConnections[connectionIdx].close();
  _activeConnectionStates[connectionIdx] = nullptr;

  --_nbActiveConnections;
#endif
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
#ifndef AERONET_WINDOWS
  // POSIX only: trim trailing null vector slots and reclaim capacity.
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
#endif
}
}  // namespace aeronet::internal