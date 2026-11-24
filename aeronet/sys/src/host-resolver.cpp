#include "aeronet/host-resolver.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "aeronet/event-fd.hpp"
#include "aeronet/log.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

HostResolution::HostResolution(std::string_view host, uint16_t port) : hostBuffer(host.size() + 1U), port(port) {
  hostBuffer.unchecked_append(host);
  hostBuffer.unchecked_push_back('\0');
}

// State shared with the resolver threads, which outlive the resolver when they are busy at its destruction.
struct HostResolver::Shared {
  std::mutex mutex;
  std::condition_variable condition;
  vector<std::shared_ptr<HostResolution>> queue;
  vector<std::shared_ptr<HostResolution>> resolved;
  const EventFd* pWakeupFd{nullptr};
  uint32_t nbThreads{0};
  uint32_t nbIdleThreads{0};
  std::atomic<bool> hasResolved{false};
  bool stopped{false};
};

HostResolver::HostResolver(const EventFd& wakeupFd) : _shared(std::make_shared<Shared>()) {
  _shared->pWakeupFd = &wakeupFd;
}

HostResolver::~HostResolver() {
  {
    std::scoped_lock lock(_shared->mutex);
    _shared->stopped = true;
    _shared->queue.clear();
  }
  _shared->condition.notify_all();
}

void HostResolver::resolve(std::shared_ptr<HostResolution> resolution) {
  std::unique_lock lock(_shared->mutex);
  _shared->queue.push_back(std::move(resolution));
  if (_shared->nbIdleThreads == 0 && _shared->nbThreads < kMaxThreads) {
    try {
      std::thread(&HostResolver::ResolveLoop, _shared).detach();
      ++_shared->nbThreads;
    } catch (const std::system_error& ex) {
      // The queued resolutions wait for a running thread. Without any, fail them right away.
      log::error("Unable to start a host resolver thread: {}", ex.what());
      if (_shared->nbThreads == 0) {
        for (auto& failed : _shared->queue) {
          _shared->resolved.push_back(std::move(failed));
        }
        _shared->queue.clear();
        _shared->hasResolved.store(true, std::memory_order_release);
        lock.unlock();
        // Called from the event loop thread: the wakeup makes it collect the failed resolutions at its next iteration.
        _shared->pWakeupFd->send();
        return;
      }
    }
  }
  lock.unlock();
  _shared->condition.notify_one();
}

void HostResolver::cancel(const HostResolution& resolution) {
  std::scoped_lock lock(_shared->mutex);
  const auto it = std::ranges::find_if(_shared->queue, [&resolution](const std::shared_ptr<HostResolution>& queued) {
    return queued.get() == &resolution;
  });
  if (it != _shared->queue.end()) {
    _shared->queue.erase(it);
  }
}

bool HostResolver::hasResolved() const noexcept { return _shared->hasResolved.load(std::memory_order_acquire); }

void HostResolver::takeResolved(vector<std::shared_ptr<HostResolution>>& out) {
  out.clear();
  std::scoped_lock lock(_shared->mutex);
  out.swap(_shared->resolved);
  _shared->hasResolved.store(false, std::memory_order_release);
}

void HostResolver::ResolveLoop(const std::shared_ptr<Shared>& shared) {
  std::unique_lock lock(shared->mutex);
  while (true) {
    ++shared->nbIdleThreads;
    shared->condition.wait(lock, [&shared] { return shared->stopped || !shared->queue.empty(); });
    --shared->nbIdleThreads;
    if (shared->stopped) {
      break;
    }
    std::shared_ptr<HostResolution> resolution = std::move(shared->queue.front());
    // A few entries (the requesters bound them), relocated by a memmove: nothing next to a resolution.
    shared->queue.erase(shared->queue.begin());
    lock.unlock();

    // The host buffer has a writable null terminator after the host, as required by ResolveTCP().
    resolution->addresses =
        ResolveTCP(std::span<char>(resolution->hostBuffer.data(), resolution->host().size()), resolution->port);

    lock.lock();
    if (shared->stopped) {
      break;
    }
    shared->resolved.push_back(std::move(resolution));
    shared->hasResolved.store(true, std::memory_order_release);
    // Under the lock: the resolver cannot be destroyed (and the event loop it wakes up stopped) meanwhile.
    shared->pWakeupFd->send();
  }
  --shared->nbThreads;
}

}  // namespace aeronet::internal
