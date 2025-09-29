#include "aeronet/multi-http-server.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "http-method-set.hpp"
#include "http-method.hpp"
#include "invalid_argument_exception.hpp"
#include "log.hpp"

namespace aeronet {

MultiHttpServer::MultiHttpServer(HttpServerConfig cfg, uint32_t threadCount)
    : _baseConfig(std::move(cfg)), _threadCount(threadCount) {
  if (_threadCount == 0) {
    throw invalid_argument("MultiHttpServer: threadCount must be >= 1");
  }
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
            return static_cast<uint32_t>(hc);
          }()) {}

MultiHttpServer::MultiHttpServer(MultiHttpServer&& other) noexcept
    : _baseConfig(std::move(other._baseConfig)),
      _threadCount(std::exchange(other._threadCount, 0)),
      _running(std::exchange(other._running, false)),
      _resolvedPort(std::exchange(other._resolvedPort, 0)),
      _globalHandler(std::move(other._globalHandler)),
      _pathHandlersEmplace(std::move(other._pathHandlersEmplace)),
      _parserErrCb(std::move(other._parserErrCb)),
      _servers(std::move(other._servers)),
      _threads(std::move(other._threads)) {}

MultiHttpServer& MultiHttpServer::operator=(MultiHttpServer&& other) noexcept {
  if (this != &other) {
    stop();
    _baseConfig = std::move(other._baseConfig);
    _threadCount = std::exchange(other._threadCount, 0);
    _running = std::exchange(other._running, false);
    _resolvedPort = std::exchange(other._resolvedPort, 0);
    _globalHandler = std::move(other._globalHandler);
    _pathHandlersEmplace = std::move(other._pathHandlersEmplace);
    _parserErrCb = std::move(other._parserErrCb);
    _servers = std::move(other._servers);
    _threads = std::move(other._threads);
  }
  return *this;
}

[[nodiscard]] std::string MultiHttpServer::AggregatedStats::json_str() const {
  std::string out;
  out.reserve(128UL * per.size());
  out.push_back('[');
  bool first = true;
  for (const auto& st : per) {
    if (!first) {
      out.push_back(',');
    } else {
      first = false;
    }
    out.append(st.json_str());
  }
  out.push_back(']');
  return out;
}

void MultiHttpServer::ensureNotStarted() const {
  if (_running) {
    throw std::logic_error("Cannot mutate configuration after start()");
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
  if (_running) {
    throw std::logic_error("MultiHttpServer already started");
  }

  log::info("MultiHttpServer starting with {} thread(s); requested port={} reusePort={} (auto-detected={})",
            _threadCount, _baseConfig.port, _baseConfig.reusePort, (_baseConfig.port == 0 ? "yes" : "no"));
  // Prepare base config: if multiple threads, enforce reusePort.
  if (_threadCount > 1 && !_baseConfig.reusePort) {
    log::debug("Enabling SO_REUSEPORT automatically for multi-instance configuration");
    _baseConfig.reusePort = true;  // override silently (documented behavior)
  }

  // Note: reserve is important here to guarantee pointer stability
  _servers.reserve(static_cast<decltype(_servers)::size_type>(_threadCount));
  _threads.reserve(static_cast<decltype(_threads)::size_type>(_threadCount));

  // Strategy for port resolution:
  //  - If user requested port 0, launch the first server, capture its assigned port, then
  //    propagate that concrete port to subsequent HttpServerConfig objects.
  //  - Otherwise use the user-specified port directly.
  uint16_t desiredPort = _baseConfig.port;

  for (std::size_t threadPos = 0; threadPos < _threadCount; ++threadPos) {
    HttpServerConfig cfg = _baseConfig;  // copy
    if (threadPos > 0) {
      // For subsequent servers, ensure we reuse the resolved port.
      cfg.port = _resolvedPort == 0 ? desiredPort : _resolvedPort;
    }
    HttpServer& srv = _servers.emplace_back(cfg);
    srv.setParserErrorCallback(_parserErrCb);
    if (_globalHandler) {
      srv.setHandler(*_globalHandler);
    } else {
      for (auto& reg : _pathHandlersEmplace) {
        srv.addPathHandler(reg.path, reg.methods, reg.handler);
      }
    }
    log::trace("Prepared underlying server instance {} (initial port value={})", threadPos, cfg.port);
  }

  // Launch threads and wait for first to resolve port if ephemeral.
  for (decltype(_servers)::size_type threadPos = 0; threadPos < _servers.size(); ++threadPos) {
    // IMPORTANT: Use a pointer captured by value. Capturing a reference to the loop-local reference variable
    // (e.g. HttpServer& srvRef = _servers[threadPos]; then [&srvRef]) would leave the lambda holding a dangling
    // reference once the loop iteration ends (undefined behavior observed as intermittent freezes).
    HttpServer* srvPtr = &_servers[threadPos];
    _threads.emplace_back([srvPtr, threadPos]() {
      log::debug("Server thread {} entering run()", threadPos);
      try {
        srvPtr->run();
      } catch (const std::exception& ex) {
        log::error("Server thread {} terminated with exception: {}", threadPos, ex.what());
      } catch (...) {
        log::error("Server thread {} terminated with unknown exception", threadPos);
      }
      log::debug("Server thread {} exiting run()", threadPos);
    });
    if (threadPos == 0) {
      // Busy-wait (short sleeps) until port resolved or timeout.
      if (_baseConfig.port == 0) {
        for (int attempt = 0; attempt < 200; ++attempt) {  // up to ~200ms
          _resolvedPort = srvPtr->port();
          if (_resolvedPort != 0) {
            _baseConfig.port = _resolvedPort;  // propagate
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      } else {
        _resolvedPort = _baseConfig.port;
      }
      log::info("Resolved listening port :{}", _resolvedPort);
    }
  }
  _running = true;
  log::info("MultiHttpServer started successfully on port :{}", _resolvedPort);
}

void MultiHttpServer::stop() {
  if (!_running) {
    return;
  }
  log::info("MultiHttpServer stopping (instances={})", _servers.size());
  for (auto& srvPtr : _servers) {
    srvPtr.stop();
  }
  // IMPORTANT LIFETIME NOTE:
  // Each server thread captures a raw pointer to its corresponding HttpServer element stored in _servers.
  // We must therefore ensure that the pointed-to HttpServer objects remain alive until after the jthreads join.
  // std::jthread joins in its destructor, so we arrange destruction order such that:
  //   1. Local 'threads' (moved from _threads) is destroyed FIRST (joins threads) while servers are still alive.
  //   2. Local 'servers' (moved from _servers) is destroyed AFTER 'threads', releasing HttpServer objects safely.
  // Destruction order is reverse of declaration order, so declare 'servers' BEFORE 'threads'.
  auto servers = std::move(_servers);  // keeps server instances alive until end of function
  auto threads = std::move(_threads);  // jthreads will join on destruction (after this scope ends)
  _running = false;
  log::info("MultiHttpServer stopped");
  // mutex unlocked at end of scope prior to join
  // join outside lock
  // (scope exit lock unlock happens here)
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
    // Merge distributions (simple sum; not deduplicating keys separately here for perf simplicity)
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
