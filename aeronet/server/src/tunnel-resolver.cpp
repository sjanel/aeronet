#include "tunnel-resolver.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

PendingTunnel::PendingTunnel(std::string_view targetHost, uint16_t targetPort, NativeHandle client, uint32_t stream,
                             std::size_t requestBytes)
    : hostBuffer(targetHost.size() + 1U),
      consumedBytes(requestBytes),
      clientFd(client),
      streamId(stream),
      port(targetPort) {
  hostBuffer.unchecked_append(targetHost);
  hostBuffer.unchecked_push_back('\0');
}

// State shared with the resolver threads, which outlive the resolver when they are busy at its destruction.
struct TunnelResolver::Shared {
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<std::shared_ptr<PendingTunnel>> queue;
  vector<std::shared_ptr<PendingTunnel>> resolved;
  std::function<void()> notify;
  std::atomic<bool> hasResolved{false};
  uint32_t nbThreads{0};
  uint32_t nbIdleThreads{0};
  bool stopped{false};
};

TunnelResolver::TunnelResolver(std::function<void()> notify) : _shared(std::make_shared<Shared>()) {
  _shared->notify = std::move(notify);
}

TunnelResolver::~TunnelResolver() {
  {
    std::scoped_lock lock(_shared->mutex);
    _shared->stopped = true;
    _shared->notify = nullptr;
    _shared->queue.clear();
  }
  _shared->condition.notify_all();
}

void TunnelResolver::resolve(std::shared_ptr<PendingTunnel> pending) {
  std::unique_lock lock(_shared->mutex);
  _shared->queue.push_back(std::move(pending));
  if (_shared->nbIdleThreads == 0 && _shared->nbThreads < kMaxThreads) {
    try {
      std::thread(&TunnelResolver::ResolveLoop, _shared).detach();
      ++_shared->nbThreads;
    } catch (const std::system_error& ex) {
      // The queued resolutions wait for a running thread. Without any, fail them right away.
      log::error("Unable to start a CONNECT resolver thread: {}", ex.what());
      if (_shared->nbThreads == 0) {
        for (auto& failed : _shared->queue) {
          _shared->resolved.push_back(std::move(failed));
        }
        _shared->queue.clear();
        _shared->hasResolved.store(true, std::memory_order_release);
        lock.unlock();
        // Called from the event loop thread: the notification makes it collect the failed tunnels at its next
        // iteration.
        _shared->notify();
        return;
      }
    }
  }
  lock.unlock();
  _shared->condition.notify_one();
}

bool TunnelResolver::hasResolved() const noexcept { return _shared->hasResolved.load(std::memory_order_acquire); }

void TunnelResolver::takeResolved(vector<std::shared_ptr<PendingTunnel>>& out) {
  out.clear();
  std::scoped_lock lock(_shared->mutex);
  out.swap(_shared->resolved);
  _shared->hasResolved.store(false, std::memory_order_release);
}

void TunnelResolver::ResolveLoop(const std::shared_ptr<Shared>& shared) {
  std::unique_lock lock(shared->mutex);
  while (true) {
    ++shared->nbIdleThreads;
    shared->condition.wait(lock, [&shared] { return shared->stopped || !shared->queue.empty(); });
    --shared->nbIdleThreads;
    if (shared->stopped) {
      break;
    }
    std::shared_ptr<PendingTunnel> pending = std::move(shared->queue.front());
    shared->queue.pop_front();
    lock.unlock();

    // The host buffer has a writable null terminator after the host, as required by ResolveTCP().
    pending->addresses = ResolveTCP(std::span<char>(pending->hostBuffer.data(), pending->host().size()), pending->port);

    lock.lock();
    if (shared->stopped) {
      break;
    }
    shared->resolved.push_back(std::move(pending));
    shared->hasResolved.store(true, std::memory_order_release);
    // Under the lock: the resolver cannot be destroyed (and the event loop it wakes up stopped) meanwhile.
    shared->notify();
  }
  --shared->nbThreads;
}

}  // namespace aeronet::internal
