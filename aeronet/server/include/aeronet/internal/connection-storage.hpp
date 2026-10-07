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

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>

#include "aeronet/async-handler-state.hpp"
#endif

namespace aeronet::internal {

class ConnectionStorage {
 public:
  // ConnectionState addresses must remain stable for the lifetime of a connection. In particular, an async handler's
  // coroutine frame retains the HttpRequestView& passed to it, while HttpRequestView itself points back to its owning
  // state and to body-stream context stored in that state. Replacing the pointer below with an index into a resizable
  // vector<ConnectionState> would therefore leave references outside ConnectionStorage dangling when the vector grows.
  // ObjectPool provides non-relocating block storage; retaining its resolved pointer here also avoids an indexed block
  // lookup on every hot-path state access.
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
  [[nodiscard]] static ConnectionIdx ConnectionItToIdx(ConnectionIt it) noexcept {
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

  [[nodiscard]] bool empty() const noexcept {
#ifdef AERONET_WINDOWS
    return _activeConnections.empty();
#else
    return _nbActiveConnections == 0;
#endif
  }

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
#elifdef AERONET_MACOS
    // macOS only. `fd` comes straight from the poller here, and macOS kqueue has been observed to
    // deliver a late event for an already-closed fd. That fd's slot may already have been nulled and
    // trimmed by shrink_to_fit(), leaving the vector shorter than the fd, so indexing begin() +
    // (fd - 1) would read past it (the raw-pointer iterator is not bounds-checked) - an out-of-bounds
    // read surfacing as an operator[] assertion in connectionState(). Map any out-of-range fd to
    // end() (invalid / negative fds wrap to a huge index and land here too) so IsValid() reports it
    // as gone, exactly how the event loop already handles a stale fd. Linux epoll needs none of this:
    // epoll_ctl(DEL) + close() reliably purge pending events, so the poller never returns a closed fd.
    const auto idx = static_cast<ConnectionIdx>(fd - 1);
    if (idx >= _activeConnections.size()) [[unlikely]] {
      return _activeConnections.end();
    }
    return _activeConnections.begin() + idx;
#else
    return _activeConnections.begin() + (fd - 1);
#endif
  }

  [[nodiscard]] std::size_t size() const {
#ifdef AERONET_WINDOWS
    return _activeConnections.size();
#else
    return _nbActiveConnections;
#endif
  }

  ConnectionState& connectionState(ConnectionIt cnxIt) noexcept {
#ifdef AERONET_WINDOWS
    return *cnxIt._it->second;
#else
    return *_activeConnectionStates[ConnectionItToIdx(cnxIt)];
#endif
  }

  ConnectionState* pConnectionState(ConnectionIt cnxIt) noexcept {
#ifdef AERONET_WINDOWS
    return cnxIt._it->second;
#else
    return _activeConnectionStates[ConnectionItToIdx(cnxIt)];
#endif
  }

  ConnectionState* pConnectionState(NativeHandle fd) noexcept {
#ifdef AERONET_WINDOWS
    auto it = _activeConnections.find(fd);
    return it != _activeConnections.end() ? it->second : nullptr;
#else
    return _activeConnectionStates[static_cast<ConnectionIdx>(fd - 1)];
#endif
  }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  AsyncHandlerStatePool& asyncHandlerStatePool() noexcept { return _asyncHandlerStatePool; }

  // Returns the iterator of the active connection with given fd and generation, or end() if it was closed since (its fd
  // possibly reused by another connection).
  ConnectionIt findConnection(NativeHandle fd, uint32_t generation) {
#ifdef AERONET_WINDOWS
    auto it = iterator(fd);
#else
    const auto idx = static_cast<ConnectionIdx>(fd - 1);
    if (idx >= _activeConnections.size() || !_activeConnections[idx]) {
      return end();
    }
    auto it = _activeConnections.begin() + idx;
#endif
    if (it == end() || connectionState(it).generation != generation) {
      return end();
    }
    return it;
  }

  // Called when the deferred work of a coroutine of a closed connection (identified by its generation) completed:
  // destroys the coroutine, kept until then, and releases the connection state once no work runs for it anymore.
  void releaseOrphanedAsyncTask(uint32_t generation, std::coroutine_handle<> handle, uint32_t maxCachedConnections);

  [[nodiscard]] bool hasOrphanedConnectionStates() const noexcept { return !_orphanedConnectionStates.empty(); }

  [[nodiscard]] std::size_t nbOrphanedConnectionStates() const noexcept { return _orphanedConnectionStates.size(); }
#endif

  std::chrono::steady_clock::time_point now;

 private:
  ConnectionState* getNewConnectionState() {
    ConnectionState* statePtr;
    if (_cachedConnectionStates.empty()) {
      statePtr = _connectionStatePool.allocateAndConstruct();
      statePtr->request._pOwnerState = statePtr;
    } else {
      statePtr = _cachedConnectionStates.back();
      _cachedConnectionStates.pop_back();
      statePtr->reset();
    }
    statePtr->lastActivity = now;
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    statePtr->generation = _nextConnectionGeneration;
    ++_nextConnectionGeneration;
#endif
    return statePtr;
  }

  // Moves a closed connection state to the cache for reuse, or releases it.
  void cacheOrRelease(ConnectionState* pConnectionState, uint32_t maxCachedConnections);

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  uint32_t _nextConnectionGeneration{0};
  AsyncHandlerStatePool _asyncHandlerStatePool;
  // States of closed connections for which deferred work still runs (see ConnectionState::hasAsyncWorkInFlight()): they
  // are kept until it completes, the work possibly using the request or the coroutine frame until then.
  vector<ConnectionState*> _orphanedConnectionStates;
#endif

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

// Check whether an iterator references a live connection. Hides the platform-specific
// validity test (flat_hash_map end-check on Windows, implicit bool on POSIX).
[[nodiscard]] inline bool IsValid([[maybe_unused]] ConnectionStorage& storage, ConnectionStorage::ConnectionIt cnxIt) {
#ifdef AERONET_WINDOWS
  return cnxIt != storage.end();
#elifdef AERONET_MACOS
  // macOS only: iterator(fd) returns end() for a stale / out-of-range fd (see there); guard it
  // before dereferencing the raw-pointer iterator.
  return cnxIt != storage.end() && static_cast<bool>(*cnxIt);
#else
  return static_cast<bool>(*cnxIt);
#endif
}

}  // namespace aeronet::internal
