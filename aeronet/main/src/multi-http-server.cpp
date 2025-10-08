#include "aeronet/multi-http-server.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-method-set.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "invalid_argument_exception.hpp"
#include "log.hpp"

namespace aeronet {

MultiHttpServer::MultiHttpServer(HttpServerConfig cfg, uint32_t threadCount)
    : _baseConfig(std::move(cfg)), _stopRequested(std::make_unique<std::atomic<bool>>(false)) {
  // Temporary diagnostic logging (can be removed once tests stabilize)
  log::debug("MultiHttpServer ctor entry: requested threadCount={} reusePort(incoming)={} ", threadCount,
             _baseConfig.reusePort);
  if (threadCount == 0) {
    throw invalid_argument("MultiHttpServer: threadCount must be >= 1");
  }
  // Prepare base config: if multiple threads, enforce reusePort.
  if (threadCount > 1 && !_baseConfig.reusePort) {
    throw invalid_argument("MultiHttpServer: reusePort must be set for multi thread MultiHttpServer");
  }

  // Create the HttpServer (and ensure port resolution if given port is 0)
  _servers.reserve(static_cast<decltype(_servers)::size_type>(threadCount));

  // Construct first server. If port was ephemeral (0), HttpServer constructor resolves it synchronously.
  // We move the base config into the first server then copy back the resolved version (with concrete port).
  _baseConfig = _servers.emplace_back(std::move(_baseConfig)).config();
}

MultiHttpServer::MultiHttpServer(HttpServerConfig cfg)
    : MultiHttpServer(
          HttpServerConfig(std::move(cfg)),  // make a copy for base storage
          []() {
            auto hc = std::thread::hardware_concurrency();
            if (hc == 0) {
              hc = 1;
              log::warn("Unable to detect the number of available processors for MultiHttpServer - defaults to {}", hc);
            }
            log::debug("MultiHttpServer auto-thread constructor detected hw_concurrency={}", hc);
            return static_cast<uint32_t>(hc);
          }()) {}

MultiHttpServer::MultiHttpServer(MultiHttpServer&& other) noexcept
    : _baseConfig(std::move(other._baseConfig)),
      _globalHandler(std::move(other._globalHandler)),
      _pathHandlersEmplace(std::move(other._pathHandlersEmplace)),
      _parserErrCb(std::move(other._parserErrCb)),
      _stopRequested(std::move(other._stopRequested)),
      _servers(std::move(other._servers)),
      _threads(std::move(other._threads)) {}

MultiHttpServer& MultiHttpServer::operator=(MultiHttpServer&& other) noexcept {
  if (this != &other) {
    // Ensure we are not leaking running threads; stop existing group first.
    stop();
    _baseConfig = std::move(other._baseConfig);
    _globalHandler = std::move(other._globalHandler);
    _pathHandlersEmplace = std::move(other._pathHandlersEmplace);
    _parserErrCb = std::move(other._parserErrCb);
    _stopRequested = std::move(other._stopRequested);
    _servers = std::move(other._servers);
    _threads = std::move(other._threads);
  }
  return *this;
}

MultiHttpServer::~MultiHttpServer() { stop(); }

std::string MultiHttpServer::AggregatedStats::json_str() const {
  std::string out;
  out.reserve(128UL * per.size());
  out.push_back('[');
  bool first = true;
  for (const auto& st : per) {
    if (first) {
      first = false;
    } else {
      out.push_back(',');
    }
    out.append(st.json_str());
  }
  out.push_back(']');
  return out;
}

void MultiHttpServer::ensureNotStarted() const {
  if (!_threads.empty()) {
    // Only disallow mutations while the server farm is actively running. After stop() (threads cleared)
    // handlers and callbacks may be adjusted prior to a restart.
    throw std::logic_error("Cannot mutate configuration while running (stop() first)");
  }
}

void MultiHttpServer::setHandler(RequestHandler handler) {
  ensureNotStarted();
  if (!_pathHandlersEmplace.empty()) {
    throw std::logic_error("Cannot set global handler after adding path handlers");
  }
  _globalHandler = std::move(handler);
}

void MultiHttpServer::addPathHandler(std::string path, const http::MethodSet& methods, const RequestHandler& handler) {
  ensureNotStarted();
  if (_globalHandler) {
    throw std::logic_error("Cannot add path handlers after setting global handler");
  }
  _pathHandlersEmplace.emplace_back(std::move(path), methods, handler);
}

void MultiHttpServer::addPathHandler(std::string path, http::Method method, const RequestHandler& handler) {
  http::MethodSet ms;
  ms.insert(method);
  addPathHandler(std::move(path), ms, handler);
}

void MultiHttpServer::setParserErrorCallback(ParserErrorCallback cb) {
  ensureNotStarted();
  _parserErrCb = std::move(cb);
}

void MultiHttpServer::start() {
  if (!_threads.empty()) {
    throw std::logic_error("MultiHttpServer already started");
  }

  // Create the remaining servers
  while (_servers.size() < _servers.capacity()) {
    _servers.emplace_back(_baseConfig);
  }

  _stopRequested->store(false, std::memory_order_relaxed);

  log::info("MultiHttpServer starting with {} thread(s); requested/base port={} reusePort={}", _servers.size(),
            _baseConfig.port, _baseConfig.reusePort);

  // Note: reserve is important here to guarantee pointer stability
  _threads.reserve(static_cast<decltype(_threads)::size_type>(_servers.size()));

  for (HttpServer& srv : _servers) {
    srv.setParserErrorCallback(_parserErrCb);
    if (_globalHandler) {
      srv.setHandler(*_globalHandler);
    } else {
      for (auto& reg : _pathHandlersEmplace) {
        srv.addPathHandler(reg.path, reg.methods, reg.handler);
      }
    }
  }

  // Launch threads (each captures a stable pointer to its HttpServer element).
  for (std::size_t threadPos = 0; threadPos < _servers.size(); ++threadPos) {
    // srvPtr and stopRequested should remain constant through the whole run duration.
    // If the MultiHttpServer has been moved, the unique_ptr and vector containers ensure that these pointers stay
    // valid.
    HttpServer* srvPtr = &_servers[static_cast<vector<HttpServer>::size_type>(threadPos)];
    std::atomic<bool>* stopRequested = _stopRequested.get();
    _threads.emplace_back([stopRequested, srvPtr, threadPos]() {
      try {
        srvPtr->runUntil([stopRequested]() { return stopRequested->load(std::memory_order_relaxed); });
      } catch (const std::exception& ex) {
        log::error("Server thread {} terminated with exception: {}", threadPos, ex.what());
      } catch (...) {
        log::error("Server thread {} terminated with unknown exception", threadPos);
      }
    });
  }
  log::info("MultiHttpServer started successfully on port :{}", port());
}

void MultiHttpServer::stop() noexcept {
  if (_threads.empty()) {
    return;
  }
  _stopRequested->store(true, std::memory_order_relaxed);
  log::info("MultiHttpServer stopping (instances={})", _servers.size());
  std::ranges::for_each(_servers, [](auto& server) { server.stop(); });

  _threads.clear();
  _servers.clear();

  log::info("MultiHttpServer stopped");
}

MultiHttpServer::AggregatedStats MultiHttpServer::stats() const {
  AggregatedStats agg;
  agg.per.reserve(_servers.size());
  for (auto& srvPtr : _servers) {
    auto st = srvPtr.stats();
    agg.total.totalBytesQueued += st.totalBytesQueued;
    agg.total.totalBytesWrittenImmediate += st.totalBytesWrittenImmediate;
    agg.total.totalBytesWrittenFlush += st.totalBytesWrittenFlush;
    agg.total.deferredWriteEvents += st.deferredWriteEvents;
    agg.total.flushCycles += st.flushCycles;
    agg.total.epollModFailures += st.epollModFailures;
    agg.total.maxConnectionOutboundBuffer =
        std::max(agg.total.maxConnectionOutboundBuffer, st.maxConnectionOutboundBuffer);
#ifdef AERONET_ENABLE_OPENSSL
    agg.total.tlsHandshakesSucceeded += st.tlsHandshakesSucceeded;
    agg.total.tlsClientCertPresent += st.tlsClientCertPresent;
    agg.total.tlsAlpnStrictMismatches += st.tlsAlpnStrictMismatches;
    for (const auto& kv : st.tlsAlpnDistribution) {
      bool found = false;
      for (auto& existing : agg.total.tlsAlpnDistribution) {
        if (existing.first == kv.first) {
          existing.second += kv.second;
          found = true;
          break;
        }
      }
      if (!found) {
        agg.total.tlsAlpnDistribution.push_back(kv);
      }
    }
    for (const auto& kv : st.tlsVersionCounts) {
      bool found = false;
      for (auto& existing : agg.total.tlsVersionCounts) {
        if (existing.first == kv.first) {
          existing.second += kv.second;
          found = true;
          break;
        }
      }
      if (!found) {
        agg.total.tlsVersionCounts.push_back(kv);
      }
    }
    for (const auto& kv : st.tlsCipherCounts) {
      bool found = false;
      for (auto& existing : agg.total.tlsCipherCounts) {
        if (existing.first == kv.first) {
          existing.second += kv.second;
          found = true;
          break;
        }
      }
      if (!found) {
        agg.total.tlsCipherCounts.push_back(kv);
      }
    }
    agg.total.tlsHandshakeDurationCount += st.tlsHandshakeDurationCount;
    agg.total.tlsHandshakeDurationTotalNs += st.tlsHandshakeDurationTotalNs;
    agg.total.tlsHandshakeDurationMaxNs = std::max(agg.total.tlsHandshakeDurationMaxNs, st.tlsHandshakeDurationMaxNs);
#endif
    agg.per.push_back(std::move(st));
  }
  log::trace("Aggregated stats across {} server instance(s)", agg.per.size());
  return agg;
}

}  // namespace aeronet
