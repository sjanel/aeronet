#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
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

#include "aeronet/async-http-server.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/test_util.hpp"
#include "bench_util.hpp"
#include "log.hpp"
#include "oatpp/core/Types.hpp"
#include "stringconv.hpp"

#ifdef AERONET_BENCH_ENABLE_DROGON
#include <drogon/drogon.h>
#endif
#ifdef AERONET_BENCH_ENABLE_OATPP
#include <oatpp/network/Server.hpp>
#include <oatpp/network/tcp/server/ConnectionProvider.hpp>
#include <oatpp/web/server/HttpConnectionHandler.hpp>
#include <oatpp/web/server/HttpRequestHandler.hpp>
#include <oatpp/web/server/HttpRouter.hpp>
#endif
#ifdef AERONET_BENCH_ENABLE_HTTPLIB
#include <httplib.h>
#endif

namespace {

inline constexpr std::string_view kHeadersBody = "OK";
inline constexpr int kMaxConnectionRetries = 5;

// NOTE: aeronet::HttpRequest::path() returns the decoded path WITHOUT the query string.
// The earlier implementation tried to parse "?size=..." off of path(), which always failed
// and produced zero-length bodies (hence bytes_per_second=0 for AeronetBodyMinMax).
// We now derive the size directly from the iterator-based queryParams() API.
std::size_t extractSizeParam(const aeronet::HttpRequest &req) {
  for (auto qp : req.queryParams()) {
    if (qp.key == "size") {
      return aeronet::StringToIntegral<std::size_t>(qp.value);
    }
  }
  throw std::runtime_error("missing size param");
}

struct AeronetServerRunner {
  aeronet::AsyncHttpServer async;

  std::mt19937_64 rng;

  AeronetServerRunner()
      : async([]() {
          aeronet::HttpServerConfig cfg{};
          cfg.maxRequestsPerConnection = 1000000;  // allow plenty of persistent reuse for benchmarks
          return cfg;
        }()) {
    aeronet::log::set_level(aeronet::log::level::err);

    async.router().setPath(benchutil::kBodyPath, aeronet::http::Method::GET, [this](const aeronet::HttpRequest &req) {
      aeronet::HttpResponse resp;
      auto sizeVal = extractSizeParam(req);
      resp.body(benchutil::randomStr(sizeVal, rng));
      return resp;
    });

    async.router().setPath(benchutil::kHeaderPath, aeronet::http::Method::GET, [this](const aeronet::HttpRequest &req) {
      aeronet::HttpResponse resp;
      for (auto sizeVal = extractSizeParam(req); sizeVal != 0; --sizeVal) {
        std::string key = benchutil::randomStr(sizeVal, rng);
        std::string value = benchutil::randomStr(sizeVal, rng);
        resp.customHeader(key, value);
      }
      resp.addCustomHeader("X-Req", req.body());
      resp.body(kHeadersBody);
      return resp;
    });
    async.start();
    // Small grace interval so first benchmark request doesn't race initial polling cycle.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  [[nodiscard]] uint16_t port() const { return async.port(); }
};

#ifdef AERONET_BENCH_ENABLE_DROGON
struct DrogonServerWrapper {
  uint16_t port_ = 18081;
  std::thread th;
  std::mt19937_64 rng;

  DrogonServerWrapper() {
    using namespace std::chrono_literals;

    // All Drogon setup must happen in one thread before run()
    auto &app = drogon::app();

    app.addListener("127.0.0.1", port_);
    drogon::app().setPipeliningRequestsNumber(1000000);
    drogon::app().setIdleConnectionTimeout(0);
    app.registerHandler(
        benchutil::kBodyPath,
        [this](const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
          std::size_t sizeVal = std::stoull(req->getParameter("size"));
          auto resp = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k200OK);
          resp->setBody(benchutil::randomStr(sizeVal, rng));
          cb(resp);
        },
        {drogon::Get});

    app.registerHandler(
        benchutil::kHeaderPath,
        [this](const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
          std::size_t sizeVal = std::stoull(req->getParameter("size"));
          auto resp = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k200OK);
          for (; sizeVal != 0; --sizeVal) {
            std::string key = benchutil::randomStr(sizeVal + 1, rng);
            std::string value = benchutil::randomStr(sizeVal, rng);
            resp->addHeader(std::move(key), std::move(value));
          }
          resp->addHeader("X-Req", std::string(req->getBody()));
          resp->setBody(std::string(kHeadersBody));
          cb(resp);
        },
        {drogon::Get});

    // Important: call 'app().run()' *after* listeners are configured,
    // and do it from the same thread that did the configuration.
    th = std::thread([] {
      drogon::app().run();  // This initializes internal managers properly
    });

    // Give it a moment to start
    std::this_thread::sleep_for(300ms);
  }

  ~DrogonServerWrapper() {
    using namespace std::chrono_literals;
    drogon::app().quit();
    for (int i = 0; i < 50 && drogon::app().isRunning(); ++i) {
      std::this_thread::sleep_for(10ms);
    }
    if (th.joinable()) {
      if (drogon::app().isRunning()) {
        th.detach();
      } else {
        th.join();
      }
    }
  }

  [[nodiscard]] auto port() const { return port_; }
};

std::once_flag drogonInitFlag;
DrogonServerWrapper *drogonServer = nullptr;
#endif

#ifdef AERONET_BENCH_ENABLE_OATPP
struct OatppBodyHandler : public oatpp::web::server::HttpRequestHandler {
  std::mt19937_64 &rng;

  explicit OatppBodyHandler(std::mt19937_64 &rng) : rng(rng) {}

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest> &req) override {
    std::size_t sizeVal = aeronet::StringToIntegral<std::size_t>(*req->getQueryParameter("size"));
    auto body = benchutil::randomStr(sizeVal, rng);
    return oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
        oatpp::web::protocol::http::Status::CODE_200, body);
  }
};

struct OatppHeadersHandler : public oatpp::web::server::HttpRequestHandler {
  std::mt19937_64 &rng;

  explicit OatppHeadersHandler(std::mt19937_64 &rng) : rng(rng) {}

  std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest> &req) override {
    std::size_t sizeVal = aeronet::StringToIntegral<std::size_t>(*req->getQueryParameter("size"));
    auto resp = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
        oatpp::web::protocol::http::Status::CODE_200, std::string(kHeadersBody));
    for (; sizeVal != 0; --sizeVal) {
      std::string key = benchutil::randomStr(sizeVal + 1, rng);
      std::string value = benchutil::randomStr(sizeVal, rng);
      resp->putOrReplaceHeader(oatpp::String(std::move(key)), oatpp::String(std::move(value)));
    }
    resp->putHeaderIfNotExists("X-Req", req->readBodyToString());
    return resp;
  }
};

struct OatppServerWrapper {
  std::shared_ptr<oatpp::network::Server> server;
  std::thread th;
  uint16_t _port = 18082;
  std::mt19937_64 rng;

  OatppServerWrapper() {
    oatpp::base::Environment::init();
    auto provider = oatpp::network::tcp::server::ConnectionProvider::createShared(
        {"127.0.0.1", _port, oatpp::network::Address::IP_4});
    auto router = oatpp::web::server::HttpRouter::createShared();
    auto bodyHandler = std::make_shared<OatppBodyHandler>(rng);
    auto headersHandler = std::make_shared<OatppHeadersHandler>(rng);

    router->route("GET", benchutil::kBodyPath, bodyHandler);
    router->route("GET", benchutil::kHeaderPath, headersHandler);

    auto handler = oatpp::web::server::HttpConnectionHandler::createShared(router);
    server = std::make_shared<oatpp::network::Server>(provider, handler);
    th = std::thread([this] { server->run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  ~OatppServerWrapper() {
    server->stop();
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
  std::mt19937_64 rng;
  httplib::Server svr;

  HttplibServerWrapper() {
    svr.set_keep_alive_max_count(1000000);  // large keep-alive reuse allowance
    svr.Get(benchutil::kBodyPath, [this](const httplib::Request &req, httplib::Response &res) {
      try {
        if (!req.has_param("size")) {
          res.status = 400;
          res.set_content("missing size", "text/plain");
          return;
        }
        std::size_t sizeVal = static_cast<std::size_t>(std::stoull(req.get_param_value("size")));
        res.set_content(benchutil::randomStr(sizeVal, rng), "text/plain");
      } catch (const std::exception &e) {
        res.status = 500;
        res.set_content(std::string("err ") + e.what(), "text/plain");
      }
    });
    svr.Get(benchutil::kHeaderPath, [this](const httplib::Request &req, httplib::Response &res) {
      try {
        if (!req.has_param("size")) {
          res.status = 400;
          res.set_content("missing size", "text/plain");
          return;
        }
        std::size_t sizeVal = static_cast<std::size_t>(std::stoull(req.get_param_value("size")));
        for (; sizeVal != 0; --sizeVal) {
          std::string key = benchutil::randomStr(sizeVal + 1, rng);
          std::string value = benchutil::randomStr(sizeVal, rng);
          res.set_header(key, value);
        }
        res.set_header("X-Req", req.body);
        res.set_content(std::string(kHeadersBody), "text/plain");
      } catch (const std::exception &e) {
        res.status = 500;
        res.set_content(std::string("err ") + e.what(), "text/plain");
      }
    });
    th = std::thread([this] { svr.listen("127.0.0.1", _port); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
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

  bool checkBodySz(std::size_t size) { return issueWithRetry(benchutil::kBodyPath, size, /*expectExact=*/true, size); }

  bool checkHeaders() {
    // Expect body of size kHeadersBody.size(), but allow >0 for flexibility (some frameworks may append CRLF nuances)
    return issueWithRetry(benchutil::kHeaderPath, kHeadersBody.size(), /*expectExact=*/false, kHeadersBody.size());
  }

  [[nodiscard]] int retryAttempts() const { return retryAttempts_; }

 private:
  bool issueWithRetry(std::string_view path, std::size_t reqSize, bool expectExact, std::size_t expectVal) {
    for (int attempt = 0; attempt < kMaxConnectionRetries; ++attempt) {
      auto len = benchutil::requestBodySize("GET", path, conn.fd(), reqSize, true);
      if (len && ((expectExact && *len == expectVal) || (!expectExact && *len > 0))) {
        if (attempt > 0) {
          // Count only successful retries (attempt 1 meaning second try)conn.fd()
          ++retryAttempts_;
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

  void reconnect() { conn = aeronet::test::ClientConnection(port_); }

  uint16_t port_;
  aeronet::test::ClientConnection conn;  // persistent connection for benchmark
  int retryAttempts_ = 0;
};

template <class Server>
void BodyMinMax(benchmark::State &state, std::string_view name, Server &server) {
  server.rng = std::mt19937_64{};
  PersistentClient client(server.port());
  std::size_t minSize = static_cast<std::size_t>(state.range(0));
  std::size_t maxSize = static_cast<std::size_t>(state.range(1));
  std::uniform_int_distribution<std::size_t> dist(minSize, maxSize);
  std::size_t totalBytes = 0;
  for ([[maybe_unused]] auto st : state) {
    std::size_t sz = dist(server.rng);
    if (!client.checkBodySz(sz)) {
      state.SkipWithError(std::format("{} request failed for size {}", name, sz));
      break;
    }
    totalBytes += sz;
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

#ifdef AERONET_BENCH_ENABLE_OATPP
void OatppBodyMinMax(benchmark::State &state) {
  OatppServerWrapper server;
  BodyMinMax(state, "oatpp", server);
}
#endif

#ifdef AERONET_BENCH_ENABLE_DROGON
void DrogonBodyMinMax(benchmark::State &state) {
  std::call_once(drogonInitFlag, [] {
    static DrogonServerWrapper drogonServerStatic;  // Construct once, main thread
    drogonServer = &drogonServerStatic;
  });

  BodyMinMax(state, "drogon", *drogonServer);
}
#endif

#ifdef AERONET_BENCH_ENABLE_HTTPLIB
void HttplibResponseBuild(benchmark::State &state) {
  std::mt19937_64 rng;
  const int numHeaders = static_cast<int>(state.range(0));
  const std::size_t bodySize = static_cast<std::size_t>(state.range(1));
  auto body = benchutil::randomStr(bodySize, rng);
  std::size_t bytesSynthesized = 0;
  for ([[maybe_unused]] auto st : state) {
    httplib::Response resp;
    resp.status = 200;
    for (int headerIndex = 0; headerIndex < numHeaders; ++headerIndex) {
      resp.set_header(std::format("X-H{:03}", headerIndex), "v");
    }
    resp.set_content(body, "text/plain");
    bytesSynthesized += body.size();
    benchmark::DoNotOptimize(resp.body.data());
  }
  if (state.iterations() > 0) {
    double iterCount = static_cast<double>(state.iterations());
    state.counters["body_bytes_per_iter"] = benchmark::Counter(static_cast<double>(bytesSynthesized) / iterCount);
  }
}
#endif

template <class Server>
void HeadersMinMax(benchmark::State &state, std::string_view name, Server &server) {
  server.rng = std::mt19937_64{};
  PersistentClient client(server.port());
  std::size_t minSize = static_cast<std::size_t>(state.range(0));
  std::size_t maxSize = static_cast<std::size_t>(state.range(1));
  std::uniform_int_distribution<std::size_t> dist(minSize, maxSize);
  for ([[maybe_unused]] auto st : state) {
    if (!client.checkHeaders()) {
      state.SkipWithError(std::format("{} request failed for headers", name));
      break;
    }
  }
  if (state.iterations() > 0) {
    state.counters["avg_retries_per_iter"] =
        benchmark::Counter(static_cast<double>(client.retryAttempts()) / static_cast<double>(state.iterations()));
  }
}

#ifdef AERONET_BENCH_ENABLE_HTTPLIB
void HttplibBodyMinMax(benchmark::State &state) {
  HttplibServerWrapper server;
  BodyMinMax(state, "httplib", server);
}

void HttplibHeadersMinMax(benchmark::State &state) {
  HttplibServerWrapper server;
  HeadersMinMax(state, "httplib", server);
}
#endif

void AeronetHeadersMinMax(benchmark::State &state) {
  AeronetServerRunner server;
  HeadersMinMax(state, "aeronet", server);
}

#ifdef AERONET_BENCH_ENABLE_OATPP
void OatppHeadersMinMax(benchmark::State &state) {
  OatppServerWrapper server;
  HeadersMinMax(state, "oatpp", server);
}
#endif

template <class Server>
void RandomNoReuse(benchmark::State &state, std::string_view name, Server &server) {
  std::size_t minSize = static_cast<std::size_t>(state.range(0));
  std::size_t maxSize = static_cast<std::size_t>(state.range(1));
  std::uniform_int_distribution<std::size_t> dist(minSize, maxSize);
  std::size_t totalBytes = 0;
  int retryCount = 0;
  for ([[maybe_unused]] auto st : state) {
    std::size_t sz = dist(server.rng);
    bool success = false;
    for (int attempt = 0; attempt < kMaxConnectionRetries && !success; ++attempt) {
      aeronet::test::ClientConnection ep(server.port());
      auto len = benchutil::requestBodySize("GET", benchutil::kBodyPath, ep.fd(), sz, false);
      if (len && *len == sz) {
        totalBytes += *len;
        if (attempt > 0) {
          ++retryCount;
        }
        success = true;
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

#ifdef AERONET_BENCH_ENABLE_HTTPLIB
void HttplibRandomNoReuse(benchmark::State &state) {
  HttplibServerWrapper server;
  RandomNoReuse(state, "httplib", server);
}
#endif

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
// Oatpp: no connection reuse variant (fresh TCP connection each request)
void OatppRandomNoReuse(benchmark::State &state) {
  OatppServerWrapper server;  // local server instance
  RandomNoReuse(state, "oatpp", server);
}

// Oatpp: response build overhead (construct outgoing response with N headers and body)
void OatppResponseBuild(benchmark::State &state) {
  std::mt19937_64 rng;
  const int numHeaders = static_cast<int>(state.range(0));
  const std::size_t bodySize = static_cast<std::size_t>(state.range(1));
  auto body = benchutil::randomStr(bodySize, rng);
  std::size_t bytesSynthesized = 0;
  for ([[maybe_unused]] auto st : state) {
    auto resp = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
        oatpp::web::protocol::http::Status::CODE_200, body);
    for (int headerIndex = 0; headerIndex < numHeaders; ++headerIndex) {
      resp->putHeader(std::format("X-H{:03}", headerIndex).c_str(), "v");
    }
    bytesSynthesized += body.size();
    benchmark::DoNotOptimize(resp.get());
  }
  if (state.iterations() > 0) {
    double iterCount = static_cast<double>(state.iterations());
    state.counters["body_bytes_per_iter"] = benchmark::Counter(static_cast<double>(bytesSynthesized) / iterCount);
  }
}

#endif

// ------------------------------------------------------------
// Additional Aeronet-specific micro benchmarks (added inline
// here for convenience to keep a single executable when
// exploring comparative behavior).
// ------------------------------------------------------------

// 1) No connection reuse: establish a fresh TCP connection for every request.
//    This highlights accept + handshake + kernel scheduling overhead vs pure keep-alive.
void AeronetRandomNoReuse(benchmark::State &state) {
  AeronetServerRunner server;  // single server instance
  RandomNoReuse(state, "aeronet", server);
}

// 2) Response build cost: construct an HttpResponse with N custom headers and body size B.
//    Args: {numHeaders, bodySize}
void AeronetResponseBuild(benchmark::State &state) {
  std::mt19937_64 rng;
  const int numHeaders = static_cast<int>(state.range(0));
  const std::size_t bodySize = static_cast<std::size_t>(state.range(1));
  auto body = benchutil::randomStr(bodySize, rng);
  std::size_t bytesSynthesized = 0;
  for ([[maybe_unused]] auto it : state) {
    aeronet::HttpResponse resp;  // 200 OK default
    // Add headers (unique keys to force append path)
    for (int headerIndex = 0; headerIndex < numHeaders; ++headerIndex) {
      resp.addCustomHeader(std::format("X-H{:03}", headerIndex), "v");
    }
    resp.body(body);
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

// Drogon: no connection reuse variant
void DrogonRandomNoReuse(benchmark::State &state) {
  std::call_once(drogonInitFlag, [] {
    static DrogonServerWrapper drogonServerStatic;  // Construct once, main thread
    drogonServer = &drogonServerStatic;
  });
  RandomNoReuse(state, "drogon", *drogonServer);
}

// Drogon: response build overhead
void DrogonResponseBuild(benchmark::State &state) {
  std::mt19937_64 rng;
  const int numHeaders = static_cast<int>(state.range(0));
  const std::size_t bodySize = static_cast<std::size_t>(state.range(1));
  auto body = benchutil::randomStr(bodySize, rng);
  std::size_t bytesSynthesized = 0;
  for ([[maybe_unused]] auto str : state) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setBody(body);
    for (int headerIndex = 0; headerIndex < numHeaders; ++headerIndex) {
      resp->addHeader(std::format("X-H{:03}", headerIndex), "v");
    }
    bytesSynthesized += body.size();
    benchmark::DoNotOptimize(resp.get());
  }
  if (state.iterations() > 0) {
    double iterCount = static_cast<double>(state.iterations());
    state.counters["body_bytes_per_iter"] = benchmark::Counter(static_cast<double>(bytesSynthesized) / iterCount);
  }
}
#endif

#define REGISTER_BODY_MIN_MAX(name) BENCHMARK(name)->Args({4, 32})->Args({32, 512})->Args({4096, 8388608})
#define REGISTER_HEADERS_MIN_MAX(name) BENCHMARK(name)->Args({1, 4})->Args({4, 16})->Args({16, 128})
#define REGISTER_RESPONSE_BUILD(name) BENCHMARK(name)->Args({4, 32})->Args({32, 512})->Args({4096, 8388608})
#define REGISTER_RANDOM_NOREUSE(name) REGISTER_BODY_MIN_MAX(name)

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
REGISTER_RANDOM_NOREUSE(AeronetRandomNoReuse);
#ifdef AERONET_BENCH_ENABLE_DROGON
REGISTER_RANDOM_NOREUSE(DrogonRandomNoReuse);
#endif
#ifdef AERONET_BENCH_ENABLE_OATPP
REGISTER_RANDOM_NOREUSE(OatppRandomNoReuse);
#endif
#ifdef AERONET_BENCH_ENABLE_HTTPLIB
REGISTER_RANDOM_NOREUSE(HttplibRandomNoReuse);
#endif

}  // namespace

BENCHMARK_MAIN();
