#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/log.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/test_util.hpp"
#include "bench_util.hpp"

#ifdef AERONET_BENCH_ENABLE_DROGON
#include <drogon/drogon.h>  // IWYU pragma: export
#endif
#ifdef AERONET_BENCH_ENABLE_OATPP
#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/network/ConnectionHandler.hpp>
#include <oatpp/network/Server.hpp>
#include <oatpp/network/tcp/server/ConnectionProvider.hpp>
#include <oatpp/web/server/AsyncHttpConnectionHandler.hpp>
#include <oatpp/web/server/HttpConnectionHandler.hpp>
#include <oatpp/web/server/HttpRequestHandler.hpp>
#include <oatpp/web/server/HttpRouter.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#endif
#ifdef AERONET_BENCH_ENABLE_HTTPLIB
#include <httplib.h>
#endif

using namespace aeronet;

namespace {

inline constexpr std::string_view kHeadersBody = "OK";
inline constexpr int kMaxConnectionRetries = 5;

// Global pregenerated pool used by the simplified benches (return pre-gen bodies/headers
// and stop when exhausted). Use a plain pool object to avoid extra indirection/allocation.
benchutil::PregenPool g_stringPool;

struct AeronetServerRunner {
  HttpServer server;

  AeronetServerRunner()
      : server([]() {
          HttpServerConfig cfg{};
          cfg.maxRequestsPerConnection = 1000000;  // allow plenty of persistent reuse for benchmarks
          cfg.maxHeaderBytes = 256UL * 1024;       // allow large headers for benchmarks
          cfg.maxBodyBytes = 1UL << 25;
          return cfg;
        }()) {
    log::set_level(log::level::err);
    server.router().setPath(http::Method::GET, benchutil::kBodyPath, [](const HttpRequest &) {
      HttpResponse resp(200);
      resp.body(g_stringPool.next());
      return resp;
    });

    server.router().setPath(http::Method::GET, benchutil::kHeaderPath, [](const HttpRequest &req) {
      HttpResponse resp;
      // Read requested header count from query param 'size'
      size_t headerCount = 0;
      for (auto qp : req.queryParams()) {
        if (qp.key == "size") {
          headerCount = StringToIntegral<std::size_t>(qp.value);
          break;
        }
      }
      if (headerCount == 0) {
        throw std::runtime_error("Should have found number of headers");
      }
      for (size_t headerPos = 0; headerPos < headerCount; ++headerPos) {
        resp.addHeader(g_stringPool.next(), g_stringPool.next());
      }
      resp.body(std::to_string(headerCount));
      return resp;
    });
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  [[nodiscard]] uint16_t port() const { return server.port(); }
};

#ifdef AERONET_BENCH_ENABLE_DROGON
struct DrogonServerWrapper {
  uint16_t _port = 18081;
  std::thread th;

  DrogonServerWrapper() {
    using namespace std::chrono_literals;

    // All Drogon setup must happen in one thread before run()
    auto &app = drogon::app();

    app.addListener("127.0.0.1", _port);
    drogon::app().setPipeliningRequestsNumber(1000000);
    drogon::app().setIdleConnectionTimeout(0);

    // Body handler
    app.registerHandler(benchutil::kBodyPath,
                        [](const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
                          auto resp = drogon::HttpResponse::newHttpResponse();
                          resp->setStatusCode(drogon::k200OK);
                          auto body = g_stringPool.next();
                          resp->addHeader("Content-Length", std::to_string(body.size()));
                          resp->setBody(std::string(std::move(body)));
                          cb(resp);
                        },
                        {drogon::Get});

    // Header handler
    app.registerHandler(
        benchutil::kHeaderPath,
        [](const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
          auto resp = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k200OK);
          auto sz = req->getParameter("size");
          size_t headerCount = static_cast<size_t>(std::stoull(sz));

          for (size_t i = 0; i < headerCount; ++i) {
            resp->addHeader(g_stringPool.next(), g_stringPool.next());
          }
          resp->setBody(std::to_string(headerCount));
          cb(resp);
        },
        {drogon::Get});

    // Important: call 'app().run()' *after* listeners are configured,
    // and do it from the same thread that did the configuration.
    // Configure Drogon to run with a single worker thread so the benchmark
    // compares single-threaded frameworks fairly. (Drogon exposes setThreadNum.)
    drogon::app().setThreadNum(1);
    th = std::thread([] { drogon::app().run(); });  // This initializes internal managers properly

    // Give it a moment to start
    std::this_thread::sleep_for(300ms);
  }

  ~DrogonServerWrapper() {
    using namespace std::chrono_literals;
    drogon::app().quit();
    for (int i = 0; i < 100 && drogon::app().isRunning(); ++i) {
      std::this_thread::sleep_for(5ms);
    }
    if (th.joinable()) {
      if (drogon::app().isRunning()) {
        th.detach();
      } else {
        th.join();
      }
    }
  }

  [[nodiscard]] auto port() const { return _port; }
};

std::once_flag drogonInitFlag;
DrogonServerWrapper *drogonServer = nullptr;
#endif

#ifdef AERONET_BENCH_ENABLE_OATPP

#include OATPP_CODEGEN_BEGIN(ApiController)  /// <-- Begin Code-Gen

class AsyncController : public oatpp::web::server::api::ApiController {
 private:
  using __ControllerType = AsyncController;

 public:
  AsyncController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
      : oatpp::web::server::api::ApiController(objectMapper) {}

  ENDPOINT_ASYNC("GET", benchutil::kBodyPath, GetBody){ENDPOINT_ASYNC_INIT(GetBody)

                                                           Action act() override{auto body = g_stringPool.next();
  auto bodySz = body.size();
  auto resp = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
      oatpp::web::protocol::http::Status::CODE_200, std::move(body));
  resp->putOrReplaceHeader("Content-Length", std::to_string(bodySz));
  return _return(resp);
}
};

ENDPOINT_ASYNC("GET", benchutil::kHeaderPath,
               GetHeader){ENDPOINT_ASYNC_INIT(GetHeader)

                              Action act() override{auto szParam = request->getQueryParameter("size");
auto resp = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
    oatpp::web::protocol::http::Status::CODE_200, szParam);
auto nbHeaders = StringToIntegral<int>(std::string_view(szParam->data(), szParam->size()));
for (int headerPos = 0; headerPos < nbHeaders; ++headerPos) {
  resp->putOrReplaceHeader(oatpp::String(g_stringPool.next()), oatpp::String(g_stringPool.next()));
}
return _return(resp);
}
}
;
}
;

#include OATPP_CODEGEN_END(ApiController)  /// <-- End Code-Gen

struct OatppBodyHandler : public oatpp::web::server::HttpRequestHandler {
  // no local RNG; pool-owned RNG is used for pregenerated strings
  OatppBodyHandler() = default;

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest> &) override {
    auto body = g_stringPool.next();
    auto bodySz = body.size();
    auto resp = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
        oatpp::web::protocol::http::Status::CODE_200, std::move(body));
    resp->putOrReplaceHeader("Content-Length", std::to_string(bodySz));
    return resp;
  }
};

struct OatppHeadersHandler : public oatpp::web::server::HttpRequestHandler {
  // no local RNG; pool-owned RNG is used for pregenerated strings
  OatppHeadersHandler() = default;

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest> &req) override {
    auto szParam = req->getQueryParameter("size");
    auto resp = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
        oatpp::web::protocol::http::Status::CODE_200, szParam);
    auto nbHeaders = StringToIntegral<int>(std::string_view(szParam->data(), szParam->size()));
    for (int headerPos = 0; headerPos < nbHeaders; ++headerPos) {
      resp->putOrReplaceHeader(oatpp::String(g_stringPool.next()), oatpp::String(g_stringPool.next()));
    }
    return resp;
  }
};

struct OatppServerWrapper {
  std::shared_ptr<oatpp::network::Server> server;
  std::shared_ptr<oatpp::network::tcp::server::ConnectionProvider> provider;
  std::shared_ptr<oatpp::network::ConnectionHandler> handler;
  std::thread th;
  uint16_t _port = 18082;

  OatppServerWrapper() {
    oatpp::base::Environment::init();

    provider = oatpp::network::tcp::server::ConnectionProvider::createShared(
        {"127.0.0.1", _port, oatpp::network::Address::IP_4});
    auto router = oatpp::web::server::HttpRouter::createShared();
    auto bodyHandler = std::make_shared<OatppBodyHandler>();
    auto headersHandler = std::make_shared<OatppHeadersHandler>();

    // router->addController(std::make_shared<AsyncController>());

    router->route("GET", benchutil::kBodyPath, bodyHandler);
    router->route("GET", benchutil::kHeaderPath, headersHandler);

    // actually this spawns a new thread for each connection. We need to migrate to AsyncHttpConnectionHandler,
    // but I could not make it work. TODO: make it fully async with 1 thread, otherwise it's not fair for the bench
    handler = oatpp::web::server::HttpConnectionHandler::createShared(router);
    // handler = oatpp::web::server::AsyncHttpConnectionHandler::createShared(router, 1);

    server = std::make_shared<oatpp::network::Server>(provider, handler);

    th = std::thread([this] { server->run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  ~OatppServerWrapper() {
    // Stop accepting new connections first
    if (provider) {
      provider->stop();
    }

    // Then stop the server if still running
    if (server && server->getStatus() == oatpp::network::Server::STATUS_RUNNING) {
      server->stop();
    }

    // Request the connection handler to stop and wait until running connections close
    if (handler) {
      handler->stop();
    }

    if (th.joinable()) {
      th.join();
    }

    oatpp::base::Environment::destroy();
  }
  [[nodiscard]] auto port() const { return _port; }
};
#endif

#ifdef AERONET_BENCH_ENABLE_HTTPLIB
struct HttplibServerWrapper {
  uint16_t _port = 18083;
  std::thread th;
  // local RNG removed; pool-owned RNG is used instead
  httplib::Server svr;

  HttplibServerWrapper() {
    svr.set_keep_alive_max_count(1000000);  // large keep-alive reuse allowance
    svr.Get(benchutil::kBodyPath, [](const httplib::Request & /*req*/, httplib::Response &res) {
      auto bodyStr = g_stringPool.next();
      auto bodySz = bodyStr.size();
      res.set_content(std::move(bodyStr), "text/plain");
      res.set_header("Content-Length", std::to_string(bodySz));
    });
    svr.Get(benchutil::kHeaderPath, [](const httplib::Request &req, httplib::Response &res) {
      const auto nbHeaders = req.get_header_value_u64("size");
      for (size_t headerCount = 0; headerCount < nbHeaders; ++headerCount) {
        res.set_header(g_stringPool.next(), g_stringPool.next());
      }

      res.set_content(std::to_string(nbHeaders), "text/plain");
    });
    th = std::thread([this] { svr.listen("127.0.0.1", _port); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  ~HttplibServerWrapper() {
    svr.stop();
    if (th.joinable()) {
      th.join();
    }
  }

  [[nodiscard]] auto port() const { return _port; }
};
#endif

// Persistent connection variant using test ClientConnection
class PersistentClient {
 public:
  explicit PersistentClient(uint16_t port) : port_(port), conn(port) {}

  bool checkBodySz(std::size_t size) { return issueWithRetry(benchutil::kBodyPath, size, true, size); }

  bool checkHeaders(std::size_t size) {
    // Expect body of size kHeadersBody.size(), but allow >0 for flexibility (some frameworks may append CRLF nuances)
    return issueWithRetry(benchutil::kHeaderPath, size, false, kHeadersBody.size());
  }

  [[nodiscard]] int retryAttempts() const { return _retryAttempts; }

 private:
  bool issueWithRetry(std::string_view path, std::size_t reqSize, bool isCheckBody, std::size_t expectVal) {
    for (int attempt = 0; attempt < kMaxConnectionRetries; ++attempt) {
      auto len = benchutil::requestBodySize("GET", path, conn.fd(), reqSize, true);
      if (!len) {
        // Failure path: reconnect and try again (unless last attempt)
        if (attempt == 0) {
          reconnect();
        }
        continue;
      }
      if ((isCheckBody && *len == expectVal) || (!isCheckBody && *len > 0)) {
        if (attempt > 0) {
          // Count only successful retries (attempt 1 meaning second try)conn.fd()
          ++_retryAttempts;
        }
        return true;
      }
      // Failure path: reconnect and try again (unless last attempt)
      if (attempt == 0) {
        reconnect();
      }
    }
    return false;
  }

  void reconnect() { conn = test::ClientConnection(port_); }

  uint16_t port_;
  test::ClientConnection conn;  // persistent connection for benchmark
  int _retryAttempts = 0;
};

template <class Server>
void BodyMinMax(benchmark::State &state, std::string_view name, Server &server) {
  // Redesigned: do not parse request params on server; instead return a pregenerated
  // body from a pool. Create a local deterministic RNG for the benchmark sequence
  // (so all frameworks see the same sample draws) and stop when pool is exhausted.
  PersistentClient client(server.port());
  std::size_t totalBytes = 0;
  const std::size_t minSize = static_cast<std::size_t>(state.range(0));
  const std::size_t maxSize = static_cast<std::size_t>(state.range(1));
  const std::size_t nbPregenCount = static_cast<std::size_t>(state.range(2));

  g_stringPool.reset(nbPregenCount, minSize, maxSize);

  for ([[maybe_unused]] auto st : state) {
    const auto expectedNextBodySize = g_stringPool.nextSize();
    if (!client.checkBodySz(expectedNextBodySize)) {
      state.SkipWithError(std::format("{} request failed while using pregenerated pool", name));
      break;
    }
    // We don't know the request's requested size; use the actual body size reported by the server
    // (the client validated non-zero already). For throughput accounting approximate with bucket.
    totalBytes += expectedNextBodySize;
  }
  state.SetBytesProcessed(static_cast<int64_t>(totalBytes));
  if (state.iterations() > 0) {
    state.counters["avg_bytes_per_iter"] =
        benchmark::Counter(static_cast<double>(totalBytes) / static_cast<double>(state.iterations()));
    state.counters["avg_retries_per_iter"] =
        benchmark::Counter(static_cast<double>(client.retryAttempts()) / static_cast<double>(state.iterations()));
  }
}

void AeronetBodyMinMax(benchmark::State &state) {
  AeronetServerRunner server;
  BodyMinMax(state, "aeronet", server);
}

#ifdef AERONET_BENCH_ENABLE_DROGON
void DrogonBodyMinMax(benchmark::State &state) {
  std::call_once(drogonInitFlag, [] {
    static DrogonServerWrapper drogonServerStatic;  // Construct once, main thread
    drogonServer = &drogonServerStatic;
  });

  BodyMinMax(state, "drogon", *drogonServer);
}
#endif

#ifdef AERONET_BENCH_ENABLE_OATPP
void OatppBodyMinMax(benchmark::State &state) {
  OatppServerWrapper server;
  BodyMinMax(state, "oatpp", server);
}
#endif

#ifdef AERONET_BENCH_ENABLE_HTTPLIB
void HttplibBodyMinMax(benchmark::State &state) {
  HttplibServerWrapper server;
  BodyMinMax(state, "httplib", server);
}
#endif

template <class Server>
void HeadersMinMax(benchmark::State &state, std::string_view name, Server &server) {
  // Use a local deterministic RNG for drawing header counts so all frameworks
  // see the same sequence across runs.
  PersistentClient client(server.port());
  const std::size_t minNbHeaders = static_cast<std::size_t>(state.range(0));
  const std::size_t maxNbHeaders = static_cast<std::size_t>(state.range(1));
  const std::size_t minHeaderSz = static_cast<std::size_t>(state.range(2));
  const std::size_t maxHeaderSz = static_cast<std::size_t>(state.range(3));
  const std::size_t nbPregenCount = static_cast<std::size_t>(state.range(4));

  g_stringPool.reset(nbPregenCount, minHeaderSz, maxHeaderSz);

  std::uniform_int_distribution<std::size_t> dist(minNbHeaders, maxNbHeaders);
  for ([[maybe_unused]] auto st : state) {
    std::size_t nbHeaders = dist(g_stringPool.rng);
    if (!client.checkHeaders(nbHeaders)) {
      state.SkipWithError(std::format("{} request failed for headers", name));
      break;
    }
  }
  if (state.iterations() > 0) {
    state.counters["avg_retries_per_iter"] =
        benchmark::Counter(static_cast<double>(client.retryAttempts()) / static_cast<double>(state.iterations()));
  }
}

void AeronetHeadersMinMax(benchmark::State &state) {
  AeronetServerRunner server;
  HeadersMinMax(state, "aeronet", server);
}

#ifdef AERONET_BENCH_ENABLE_DROGON
void DrogonHeadersMinMax(benchmark::State &state) {
  std::call_once(drogonInitFlag, [] {
    static DrogonServerWrapper drogonServerStatic;  // Construct once, main thread
    drogonServer = &drogonServerStatic;
  });
  HeadersMinMax(state, "drogon", *drogonServer);
}
#endif

#ifdef AERONET_BENCH_ENABLE_OATPP
void OatppHeadersMinMax(benchmark::State &state) {
  OatppServerWrapper server;
  HeadersMinMax(state, "oatpp", server);
}
#endif

#ifdef AERONET_BENCH_ENABLE_HTTPLIB
void HttplibHeadersMinMax(benchmark::State &state) {
  HttplibServerWrapper server;
  HeadersMinMax(state, "httplib", server);
}
#endif

template <class Server>
void BodyMinMaxNoReuse(benchmark::State &state, std::string_view name, Server &server) {
  const std::size_t minSize = static_cast<std::size_t>(state.range(0));
  const std::size_t maxSize = static_cast<std::size_t>(state.range(1));
  const std::size_t nbPregenCount = static_cast<std::size_t>(state.range(2));

  g_stringPool.reset(nbPregenCount, minSize, maxSize);

  int retryCount = 0;

  std::size_t totalBytes = 0;
  for ([[maybe_unused]] auto st : state) {
    bool success = false;
    for (int attempt = 0; attempt < kMaxConnectionRetries && !success; ++attempt) {
      const auto expectedNextBodySize = g_stringPool.nextSize();

      test::ClientConnection ep(server.port());
      auto len = benchutil::requestBodySize("GET", benchutil::kBodyPath, ep.fd(), expectedNextBodySize, false);
      if (len && *len == expectedNextBodySize) {
        if (attempt > 0) {
          ++retryCount;
        }
        success = true;
        totalBytes += expectedNextBodySize;
        break;
      }
    }
    if (!success) {
      state.SkipWithError(std::format("{} no-reuse request failed", name));
      break;
    }
  }
  state.SetBytesProcessed(static_cast<int64_t>(totalBytes));
  if (state.iterations() > 0) {
    double iterCount = static_cast<double>(state.iterations());
    state.counters["avg_bytes_per_iter"] = benchmark::Counter(static_cast<double>(totalBytes) / iterCount);
    state.counters["avg_retries_per_iter"] = benchmark::Counter(static_cast<double>(retryCount) / iterCount);
  }
}

// 1) No connection reuse: establish a fresh TCP connection for every request.
//    This highlights accept + handshake + kernel scheduling overhead vs pure keep-alive.
void AeronetBodyMinMaxNoReuse(benchmark::State &state) {
  AeronetServerRunner server;  // single server instance
  BodyMinMaxNoReuse(state, "aeronet", server);
}

#ifdef AERONET_BENCH_ENABLE_DROGON
void DrogonBodyMinMaxNoReuse(benchmark::State &state) {
  std::call_once(drogonInitFlag, [] {
    static DrogonServerWrapper drogonServerStatic;  // Construct once, main thread
    drogonServer = &drogonServerStatic;
  });
  BodyMinMaxNoReuse(state, "drogon", *drogonServer);
}
#endif

#ifdef AERONET_BENCH_ENABLE_OATPP
// Oatpp: no connection reuse variant (fresh TCP connection each request)
void OatppBodyMinMaxNoReuse(benchmark::State &state) {
  OatppServerWrapper server;  // local server instance
  BodyMinMaxNoReuse(state, "oatpp", server);
}
#endif

#ifdef AERONET_BENCH_ENABLE_HTTPLIB
void HttplibBodyMinMaxNoReuse(benchmark::State &state) {
  HttplibServerWrapper server;
  BodyMinMaxNoReuse(state, "httplib", server);
}
#endif

void AeronetResponseBuild(benchmark::State &state) {
  const std::size_t minNbHeaders = static_cast<std::size_t>(state.range(0));
  const std::size_t maxNbHeaders = static_cast<std::size_t>(state.range(1));
  const std::size_t minSize = static_cast<std::size_t>(state.range(2));
  const std::size_t maxSize = static_cast<std::size_t>(state.range(3));
  const std::size_t nbPregenCount = static_cast<std::size_t>(state.range(4));

  g_stringPool.reset(nbPregenCount, minSize, maxSize);

  std::uniform_int_distribution<std::size_t> dist(minNbHeaders, maxNbHeaders);

  std::size_t bytesSynthesized = 0;
  for ([[maybe_unused]] auto it : state) {
    const auto numHeaders = dist(g_stringPool.rng);

    HttpResponse resp(200);

    auto body = g_stringPool.next();

    for (std::size_t headerIndex = 0; headerIndex < numHeaders; ++headerIndex) {
      auto headerKey = g_stringPool.next();
      auto headerVal = g_stringPool.next();
      bytesSynthesized += headerKey.size() + headerVal.size();

      resp.addHeader(headerKey, headerVal);
    }
    resp.body(std::move(body));
    // Finalization occurs when serialized for send; emulate by calling body() + reserved header injection via copy
    // We approximate work by measuring the total constructed response buffer size.
    bytesSynthesized += resp.body().size();
    benchmark::DoNotOptimize(resp.body().data());
  }
  if (state.iterations() > 0) {
    double iterCount = static_cast<double>(state.iterations());
    state.counters["body_bytes_per_iter"] = benchmark::Counter(static_cast<double>(bytesSynthesized) / iterCount);
  }
}

#ifdef AERONET_BENCH_ENABLE_DROGON
void DrogonResponseBuild(benchmark::State &state) {
  const std::size_t minNbHeaders = static_cast<std::size_t>(state.range(0));
  const std::size_t maxNbHeaders = static_cast<std::size_t>(state.range(1));
  const std::size_t minSize = static_cast<std::size_t>(state.range(2));
  const std::size_t maxSize = static_cast<std::size_t>(state.range(3));
  const std::size_t nbPregenCount = static_cast<std::size_t>(state.range(4));

  g_stringPool.reset(nbPregenCount, minSize, maxSize);

  std::uniform_int_distribution<std::size_t> dist(minNbHeaders, maxNbHeaders);

  std::size_t bytesSynthesized = 0;
  for ([[maybe_unused]] auto str : state) {
    const auto numHeaders = dist(g_stringPool.rng);

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setBody(g_stringPool.next());
    for (std::size_t headerIndex = 0; headerIndex < numHeaders; ++headerIndex) {
      auto headerKey = g_stringPool.next();
      auto headerVal = g_stringPool.next();
      bytesSynthesized += headerKey.size() + headerVal.size();
      resp->addHeader(std::move(headerKey), std::move(headerVal));
    }
    bytesSynthesized += resp->getBody().size();
    benchmark::DoNotOptimize(resp.get());
  }
  if (state.iterations() > 0) {
    double iterCount = static_cast<double>(state.iterations());
    state.counters["body_bytes_per_iter"] = benchmark::Counter(static_cast<double>(bytesSynthesized) / iterCount);
  }
}
#endif

#ifdef AERONET_BENCH_ENABLE_OATPP
void OatppResponseBuild(benchmark::State &state) {
  const std::size_t minNbHeaders = static_cast<std::size_t>(state.range(0));
  const std::size_t maxNbHeaders = static_cast<std::size_t>(state.range(1));
  const std::size_t minSize = static_cast<std::size_t>(state.range(2));
  const std::size_t maxSize = static_cast<std::size_t>(state.range(3));
  const std::size_t nbPregenCount = static_cast<std::size_t>(state.range(4));

  g_stringPool.reset(nbPregenCount, minSize, maxSize);

  std::uniform_int_distribution<std::size_t> dist(minNbHeaders, maxNbHeaders);

  std::size_t bytesSynthesized = 0;
  for ([[maybe_unused]] auto st : state) {
    const auto numHeaders = dist(g_stringPool.rng);

    auto body = g_stringPool.next();
    auto bodySz = body.size();
    auto resp = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
        oatpp::web::protocol::http::Status::CODE_200, std::move(body));
    for (std::size_t headerIndex = 0; headerIndex < numHeaders; ++headerIndex) {
      auto headerKey = g_stringPool.next();
      auto headerVal = g_stringPool.next();
      bytesSynthesized += headerKey.size() + headerVal.size();
      resp->putHeader(std::move(headerKey), std::move(headerVal));
    }
    bytesSynthesized += bodySz;
    benchmark::DoNotOptimize(resp.get());
  }
  if (state.iterations() > 0) {
    double iterCount = static_cast<double>(state.iterations());
    state.counters["body_bytes_per_iter"] = benchmark::Counter(static_cast<double>(bytesSynthesized) / iterCount);
  }
}
#endif

#ifdef AERONET_BENCH_ENABLE_HTTPLIB
void HttplibResponseBuild(benchmark::State &state) {
  const std::size_t minNbHeaders = static_cast<std::size_t>(state.range(0));
  const std::size_t maxNbHeaders = static_cast<std::size_t>(state.range(1));
  const std::size_t minSize = static_cast<std::size_t>(state.range(2));
  const std::size_t maxSize = static_cast<std::size_t>(state.range(3));
  const std::size_t nbPregenCount = static_cast<std::size_t>(state.range(4));

  g_stringPool.reset(nbPregenCount, minSize, maxSize);

  std::uniform_int_distribution<std::size_t> dist(minNbHeaders, maxNbHeaders);

  std::size_t bytesSynthesized = 0;
  for ([[maybe_unused]] auto st : state) {
    const auto numHeaders = dist(g_stringPool.rng);

    httplib::Response resp;
    resp.status = 200;
    resp.set_content(g_stringPool.next(), "text/plain");

    for (std::size_t headerIndex = 0; headerIndex < numHeaders; ++headerIndex) {
      auto headerKey = g_stringPool.next();
      auto headerVal = g_stringPool.next();
      bytesSynthesized += headerKey.size() + headerVal.size();

      resp.set_header(std::move(headerKey), std::move(headerVal));
    }
    bytesSynthesized += resp.body.size();
    benchmark::DoNotOptimize(resp.body.data());
  }
  if (state.iterations() > 0) {
    double iterCount = static_cast<double>(state.iterations());
    state.counters["body_bytes_per_iter"] = benchmark::Counter(static_cast<double>(bytesSynthesized) / iterCount);
  }
}
#endif

#define REGISTER_BODY_MIN_MAX(name) \
  BENCHMARK(name)->Args({4, 32, (1 << 17)})->Args({32, 512, (1 << 16)})->Args({4096, 8388608, (1 << 10)})
#define REGISTER_HEADERS_MIN_MAX(name) \
  BENCHMARK(name)->Args({2, 8, 4, 8, (1 << 17)})->Args({16, 64, 4, 32, (1 << 16)})->Args({128, 1024, 4, 128, (1 << 10)})
#define REGISTER_RESPONSE_BUILD(name)   \
  BENCHMARK(name)                       \
      ->Args({1, 2, 4, 8, (1 << 17)})   \
      ->Args({4, 8, 16, 64, (1 << 16)}) \
      ->Args({16, 64, 32, (1 << 16), (1 << 10)})
#define REGISTER_BODY_MIN_MAX_NOREUSE(name) REGISTER_BODY_MIN_MAX(name)

// Body min/max across frameworks
REGISTER_BODY_MIN_MAX(AeronetBodyMinMax);
#ifdef AERONET_BENCH_ENABLE_DROGON
REGISTER_BODY_MIN_MAX(DrogonBodyMinMax);
#endif
#ifdef AERONET_BENCH_ENABLE_OATPP
REGISTER_BODY_MIN_MAX(OatppBodyMinMax);
#endif
#ifdef AERONET_BENCH_ENABLE_HTTPLIB
REGISTER_BODY_MIN_MAX(HttplibBodyMinMax);
#endif

// Headers min/max
REGISTER_HEADERS_MIN_MAX(AeronetHeadersMinMax);
#ifdef AERONET_BENCH_ENABLE_DROGON
REGISTER_HEADERS_MIN_MAX(DrogonHeadersMinMax);
#endif
#ifdef AERONET_BENCH_ENABLE_OATPP
REGISTER_HEADERS_MIN_MAX(OatppHeadersMinMax);
#endif
#ifdef AERONET_BENCH_ENABLE_HTTPLIB
REGISTER_HEADERS_MIN_MAX(HttplibHeadersMinMax);
#endif

// Response build overhead
REGISTER_RESPONSE_BUILD(AeronetResponseBuild);
#ifdef AERONET_BENCH_ENABLE_DROGON
REGISTER_RESPONSE_BUILD(DrogonResponseBuild);
#endif
#ifdef AERONET_BENCH_ENABLE_OATPP
REGISTER_RESPONSE_BUILD(OatppResponseBuild);
#endif
#ifdef AERONET_BENCH_ENABLE_HTTPLIB
REGISTER_RESPONSE_BUILD(HttplibResponseBuild);
#endif

// No-reuse benchmarks reuse body arg pattern
REGISTER_BODY_MIN_MAX_NOREUSE(AeronetBodyMinMaxNoReuse);
#ifdef AERONET_BENCH_ENABLE_DROGON
REGISTER_BODY_MIN_MAX_NOREUSE(DrogonBodyMinMaxNoReuse);
#endif
#ifdef AERONET_BENCH_ENABLE_OATPP
REGISTER_BODY_MIN_MAX_NOREUSE(OatppBodyMinMaxNoReuse);
#endif
#ifdef AERONET_BENCH_ENABLE_HTTPLIB
REGISTER_BODY_MIN_MAX_NOREUSE(HttplibBodyMinMaxNoReuse);
#endif

}  // namespace

BENCHMARK_MAIN();
