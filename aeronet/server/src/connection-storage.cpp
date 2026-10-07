#include "aeronet/internal/connection-storage.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>
#endif

#include "aeronet/connection-state.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-transport.hpp"
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/async-handler-state.hpp"
#include "aeronet/protocol-handler.hpp"
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
  // Deferred work still running may use the request and the coroutine frame: keep them until it completes.
  const bool keepForAsyncWork = pConnectionState->hasAsyncWorkInFlight();
  if (keepForAsyncWork) {
    if (auto* asyncState = pConnectionState->asyncState.get(); asyncState != nullptr) {
      asyncState->active = false;
      asyncState->pendingResponse.reset();
    }
  } else if (auto* asyncState = pConnectionState->asyncState.get(); asyncState != nullptr) {
    asyncState->clear();
    pConnectionState->asyncState.reset();
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

    if (auto* tlsTr = pConnectionState->transport.get<TlsTransport>()) {
      tlsTr->shutdown();
    }
  }
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  if (keepForAsyncWork) {
    _orphanedConnectionStates.push_back(pConnectionState);
  } else {
    cacheOrRelease(pConnectionState, maxCachedConnections);
  }
#else
  cacheOrRelease(pConnectionState, maxCachedConnections);
#endif

#ifdef AERONET_WINDOWS
  // Do NOT call cnxIt._it->first.close() here: Connection is the hash-map key,
  // and mutating the fd (which changes the hash) before erase corrupts the
  // internal chain structure of bytell_hash_map, causing infinite loops in
  // find_parent_block(). The erase will destroy the pair whose Connection
  // destructor (~BaseFd) closes the socket automatically.
  _activeConnections.erase(cnxIt._it);
#else
  _activeConnections[connectionIdx].close();
  _activeConnectionStates[connectionIdx] = nullptr;

  --_nbActiveConnections;
#endif
}

void ConnectionStorage::cacheOrRelease(ConnectionState* pConnectionState, uint32_t maxCachedConnections) {
  // Move ConnectionState to cache for potential reuse
  if (_cachedConnectionStates.size() < maxCachedConnections) {
    _cachedConnectionStates.push_back(pConnectionState);
  } else {
    _connectionStatePool.destroyAndRelease(pConnectionState);
  }
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
void ConnectionStorage::releaseOrphanedAsyncTask(uint32_t generation, std::coroutine_handle<> handle,
                                                 uint32_t maxCachedConnections) {
  const auto it = std::ranges::find_if(_orphanedConnectionStates, [generation](const ConnectionState* pState) {
    return pState->generation == generation;
  });
  if (it == _orphanedConnectionStates.end()) {
    return;
  }
  ConnectionState* pConnectionState = *it;
  if (auto* asyncState = pConnectionState->asyncState.get(); asyncState != nullptr && asyncState->handle == handle) {
    asyncState->clear();
  }
  if (pConnectionState->protocolHandler != nullptr) {
    pConnectionState->protocolHandler->dropAsyncTask(handle);
  }
  if (pConnectionState->hasAsyncWorkInFlight()) {
    return;
  }
  pConnectionState->asyncState.reset();
  _orphanedConnectionStates.erase(it);
  // Kept in the cache in last activity order (see sweepCachedConnections()).
  pConnectionState->lastActivity = now;
  cacheOrRelease(pConnectionState, maxCachedConnections);
}
#endif

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
