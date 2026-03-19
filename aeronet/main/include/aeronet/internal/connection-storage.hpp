#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_WINDOWS
#include <functional>

#include "aeronet/flat-hash-map.hpp"
#endif

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
#ifdef AERONET_WINDOWS
  using ConnectionMap = flat_hash_map<Connection, ConnectionState*, std::hash<NativeHandle>, std::equal_to<>>;
#else
  using ConnectionVector = vector<Connection>;
  using ConnectionIdx = ConnectionVector::size_type;
#endif

#ifdef AERONET_WINDOWS
  // Thin wrapper giving flat_hash_map::iterator the same dereference semantics as vector<Connection>::iterator.
  class ConnectionIt {
   public:
    ConnectionIt() = default;

    Connection& operator*() const { return _it->first; }
    Connection* operator->() const { return &_it->first; }
    ConnectionIt& operator++() {
      ++_it;
      return *this;
    }
    ConnectionIt operator++(int) {
      auto ret = *this;
      ++_it;
      return ret;
    }

    bool operator==(const ConnectionIt&) const = default;

   private:
    friend class ConnectionStorage;

    explicit ConnectionIt(ConnectionMap::iterator it) : _it(it) {}

    ConnectionMap::iterator _it{};
  };
#else
  using ConnectionIt = ConnectionVector::iterator;
#endif

 private:
#ifndef AERONET_WINDOWS
  [[nodiscard]] static ConnectionIdx ConnectionItToIdx(ConnectionIt it) {
    return static_cast<ConnectionIdx>(it->fd() - 1);
  }
#endif

 public:
#ifdef AERONET_ENABLE_OPENSSL
  void recycleOrRelease(ConnectionIt cnxIt, uint32_t maxCachedConnections, bool tlsEnabled,
                        uint32_t& handshakesInFlight);
#else
  void recycleOrRelease(ConnectionIt cnxIt, uint32_t maxCachedConnections);
#endif

  ConnectionIt emplace(Connection&& cnx) {
#ifdef AERONET_WINDOWS
    auto [it, inserted] = _activeConnections.emplace(std::move(cnx), getNewConnectionState());
    assert(inserted);
    return ConnectionIt(it);
#else
    const NativeHandle fd = cnx.fd();
    assert(fd != 0);
    const auto connectionIdx = static_cast<ConnectionIdx>(fd - 1);

    while (_activeConnections.size() < connectionIdx + 1U) {
      _activeConnections.emplace_back();
      _activeConnectionStates.emplace_back();
    }

    _activeConnections[connectionIdx] = std::move(cnx);
    _activeConnectionStates[connectionIdx] = getNewConnectionState();
    ++_nbActiveConnections;

    return _activeConnections.begin() + connectionIdx;
#endif
  }

  void sweepCachedConnections(std::chrono::steady_clock::duration timeout);

  void shrink_to_fit();

  [[nodiscard]] auto nbCachedConnections() const noexcept { return _cachedConnectionStates.size(); }

  ConnectionIt begin() {
#ifdef AERONET_WINDOWS
    return ConnectionIt(_activeConnections.begin());
#else
    return _activeConnections.begin();
#endif
  }

  ConnectionIt end() {
#ifdef AERONET_WINDOWS
    return ConnectionIt(_activeConnections.end());
#else
    return _activeConnections.end();
#endif
  }

  ConnectionIt iterator(NativeHandle fd) {
#ifdef AERONET_WINDOWS
    return ConnectionIt(_activeConnections.find(fd));
#else
    return _activeConnections.begin() + (fd - 1);
#endif
  }

  // Check whether a ConnectionIt points to a valid active connection.
  // POSIX: checks the vector slot is occupied. Windows: checks the iterator was found in the map.
#ifdef AERONET_WINDOWS
  [[nodiscard]] bool active(ConnectionIt cnxIt) const { return cnxIt._it != _activeConnections.end(); }
#else
  [[nodiscard]] static bool active(ConnectionIt cnxIt) { return static_cast<bool>(*cnxIt); }
#endif

  [[nodiscard]] std::size_t size() const {
#ifdef AERONET_WINDOWS
    return _activeConnections.size();
#else
    return _nbActiveConnections;
#endif
  }

  ConnectionState& connectionState(ConnectionIt cnxIt) {
#ifdef AERONET_WINDOWS
    return *cnxIt._it->second;
#else
    return *_activeConnectionStates[ConnectionItToIdx(cnxIt)];
#endif
  }

  ConnectionState* pConnectionState(ConnectionIt cnxIt) {
#ifdef AERONET_WINDOWS
    return cnxIt._it->second;
#else
    return _activeConnectionStates[ConnectionItToIdx(cnxIt)];
#endif
  }

  ConnectionState* pConnectionState(NativeHandle fd) {
#ifdef AERONET_WINDOWS
    auto it = _activeConnections.find(fd);
    return it != _activeConnections.end() ? it->second : nullptr;
#else
    return _activeConnectionStates[static_cast<ConnectionIdx>(fd - 1)];
#endif
  }

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

#ifdef AERONET_WINDOWS
  ConnectionMap _activeConnections;
#else
  ConnectionVector _activeConnections;
  vector<ConnectionState*> _activeConnectionStates;
  ConnectionVector::size_type _nbActiveConnections{};
#endif
  vector<ConnectionState*> _cachedConnectionStates;  // cache of closed ConnectionState objects for reuse
};

}  // namespace aeronet::internal