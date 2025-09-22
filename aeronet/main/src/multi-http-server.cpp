#include "aeronet/multi-http-server.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "aeronet/server.hpp"
#include "invalid_argument_exception.hpp"
#include "log.hpp"

namespace aeronet {

MultiHttpServer::MultiHttpServer(ServerConfig cfg, std::size_t threadCount)
    : _baseConfig(std::move(cfg)), _threadCount(threadCount) {
  if (threadCount == 0) {
    throw invalid_argument("MultiHttpServer: threadCount must be >= 1");
  }
}

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

  log::info("MultiHttpServer starting with {} thread(s); requested port={} reusePort={}", _threadCount,
            _baseConfig.port, _baseConfig.reusePort);
  // Prepare base config: if multiple threads, enforce reusePort.
  if (_threadCount > 1 && !_baseConfig.reusePort) {
    log::debug("Enabling SO_REUSEPORT automatically for multi-instance configuration");
    _baseConfig.reusePort = true;  // override silently (documented behavior)
  }

  _servers.reserve(_threadCount);
  _threads.reserve(_threadCount);

  // Strategy for port resolution:
  //  - If user requested port 0, launch the first server, capture its assigned port, then
  //    propagate that concrete port to subsequent ServerConfig objects.
  //  - Otherwise use the user-specified port directly.
  uint16_t desiredPort = _baseConfig.port;

  for (std::size_t threadPos = 0; threadPos < _threadCount; ++threadPos) {
    ServerConfig cfg = _baseConfig;  // copy
    if (threadPos > 0) {
      // For subsequent servers, ensure we reuse the resolved port.
      cfg.port = _resolvedPort == 0 ? desiredPort : _resolvedPort;
    }
    HttpServer& srv = _servers.emplace_back(cfg);
    if (_parserErrCb) {
      srv.setParserErrorCallback(_parserErrCb);
    }
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
  for (std::size_t threadPos = 0; threadPos < _servers.size(); ++threadPos) {
    HttpServer& srvPtr = _servers[threadPos];
    _threads.emplace_back([this, &srvPtr, threadPos]() {
      log::debug("Server thread {} entering run()", threadPos);
      try {
        srvPtr.run();
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
          _resolvedPort = srvPtr.port();
          if (_resolvedPort != 0) {
            _baseConfig.port = _resolvedPort;  // propagate
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      } else {
        _resolvedPort = _baseConfig.port;
      }
      log::info("Resolved listening port: {}", _resolvedPort);
    }
  }
  _running = true;
  log::info("MultiHttpServer started successfully on port {}", _resolvedPort);
}

void MultiHttpServer::stop() {
  if (!_running) {
    return;
  }
  log::info("MultiHttpServer stopping (instances={})", _servers.size());
  for (auto& srvPtr : _servers) {
    srvPtr.stop();
  }
  // Unlock while joining to avoid potential deadlocks if join path queries server state.
  auto threads = std::move(_threads);
  auto servers = std::move(_servers);
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
    agg.total.maxConnectionOutboundBuffer =
        std::max(agg.total.maxConnectionOutboundBuffer, st.maxConnectionOutboundBuffer);
    agg.per.push_back(st);
  }
  log::trace("Aggregated stats across {} server instance(s)", agg.per.size());
  return agg;
}

}  // namespace aeronet
