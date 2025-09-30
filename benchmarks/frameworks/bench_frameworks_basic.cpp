#include <arpa/inet.h>
#include <benchmark/benchmark.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "aeronet/async-http-server.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "bench_util.hpp"
#include "log.hpp"

#ifdef HAVE_BENCH_DROGON
#include <drogon/drogon.h>
#endif
#ifdef HAVE_BENCH_OATPP
#include <oatpp/core/macro/component.hpp>
#include <oatpp/network/Server.hpp>
#include <oatpp/network/tcp/server/ConnectionProvider.hpp>
#include <oatpp/web/server/HttpConnectionHandler.hpp>
#include <oatpp/web/server/api/ApiController.hpp>
#endif

namespace {

std::string makeIota(std::size_t n) {
  std::string out;
  out.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = static_cast<char>('a' + (i % 26));
  }
  return out;
}

std::optional<std::size_t> parseSizeParam(std::string_view target) {
  auto qpos = target.find('?');
  if (qpos == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view query = target.substr(qpos + 1);
  auto key = std::string_view{"size="};
  auto kpos = query.find(key);
  if (kpos == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view rest = query.substr(kpos + key.size());
  std::size_t val = 0;
  for (char ch : rest) {
    if (ch < '0' || ch > '9') {
      break;
    }
    val = (val * 10) + static_cast<std::size_t>(ch - '0');
    if (val > (1U << 26)) {  // hard cap ~67MB
      break;
    }
  }
  return val;
}

struct AeronetServerRunner {
  aeronet::AsyncHttpServer async;
  AeronetServerRunner() : async(aeronet::HttpServer(aeronet::HttpServerConfig{})) {
    aeronet::log::set_level(aeronet::log::level::off);
    async.server().setHandler([](const aeronet::HttpRequest &req) {
      aeronet::HttpResponse resp;
      auto sizeOpt = parseSizeParam(req.path());
      resp.body(makeIota(sizeOpt.value_or(0)));
      return resp;
    });
    async.start();
    // Small grace interval so first benchmark request doesn't race initial polling cycle.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ~AeronetServerRunner() {
    async.requestStop();
    async.stopAndJoin();
  }
  [[nodiscard]] uint16_t port() const { return async.server().port(); }
};

#ifdef HAVE_BENCH_DROGON
struct DrogonServerWrapper {
  uint16_t port_ = 18081;  // fixed port (simple)
  std::thread th;
  DrogonServerWrapper() {
    using namespace std::chrono_literals;
    drogon::app().addListener("127.0.0.1", port_);
    drogon::app().registerHandler(
        "/data",
        [](const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
          std::size_t n = 0;
          try {
            auto p = req->getParameter("size");
            if (!p.empty()) n = std::stoull(p);
          } catch (...) {
          }
          auto resp = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k200OK);
          resp->setBody(makeIota(n));
          cb(resp);
        },
        {drogon::Get});
    th = std::thread([] { drogon::app().run(); });
    std::this_thread::sleep_for(300ms);
  }
  ~DrogonServerWrapper() {
    using namespace std::chrono_literals;
    drogon::app().quit();
    for (int i = 0; i < 50 && drogon::app().isRunning(); ++i) std::this_thread::sleep_for(10ms);
    if (th.joinable()) {
      if (drogon::app().isRunning())
        th.detach();
      else
        th.join();
    }
  }
  uint16_t port() const { return port_; }
};
#endif

#ifdef HAVE_BENCH_OATPP
class DataController : public oatpp::web::server::api::ApiController {
 public:
  explicit DataController(const std::shared_ptr<ObjectMapper> &objectMapper)
      : oatpp::web::server::api::ApiController(objectMapper) {}
  ENDPOINT("GET", "/data", getData, QUERY(std::string, size)) {
    std::size_t n = 0;
    try {
      if (!size.empty()) n = std::stoull(size);
    } catch (...) {
    }
    return createResponse(oatpp::web::protocol::http::Status::CODE_200, makeIota(n));
  }
};

struct OatppServerWrapper {
  std::shared_ptr<oatpp::network::Server> server;
  std::thread th;
  uint16_t port_ = 18082;
  OatppServerWrapper() {
    oatpp::base::Environment::init();
    auto provider = oatpp::network::tcp::server::ConnectionProvider::createShared(
        {"127.0.0.1", port_, oatpp::network::Address::IP_4});
    auto router = oatpp::web::server::HttpRouter::createShared();
    auto controller = std::make_shared<DataController>(nullptr);
    controller->addEndpointsToRouter(router);
    auto handler = oatpp::web::server::HttpConnectionHandler::createShared(router);
    server = std::make_shared<oatpp::network::Server>(provider, handler);
    th = std::thread([this] { server->run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  ~OatppServerWrapper() {
    server->stop();
    if (th.joinable()) th.join();
    oatpp::base::Environment::destroy();
  }
  uint16_t port() const { return port_; }
};
#endif

// Persistent connection variant using test ClientConnection
class PersistentClient {
 public:
  explicit PersistentClient(uint16_t port) : conn(port) {}
  bool getSize(std::size_t size, std::size_t &outLen) {
    auto len = benchutil::requestBodySize(conn, size);
    if (!len) {
      return false;
    }
    outLen = *len;
    return true;
  }

 private:
  ClientConnection conn;  // from test_util.hpp
};

void BenchAeronetRandom(benchmark::State &state) {
  AeronetServerRunner server;
  PersistentClient client(server.port());
  std::size_t minSize = static_cast<std::size_t>(state.range(0));
  std::size_t maxSize = static_cast<std::size_t>(state.range(1));
  std::mt19937_64 rng{123456789ULL};
  std::uniform_int_distribution<std::size_t> dist(minSize, maxSize);
  std::size_t totalBytes = 0;
  for (auto it : state) {
    (void)it;
    std::size_t sz = dist(rng);
    std::size_t respLen = 0;
    if (!client.getSize(sz, respLen)) {
      state.SkipWithError("aeronet request failed");
      break;
    }
    totalBytes += respLen;
  }
  state.SetBytesProcessed(static_cast<int64_t>(totalBytes));
  if (state.iterations() > 0) {
    state.counters["avg_bytes_per_iter"] =
        benchmark::Counter(static_cast<double>(totalBytes) / static_cast<double>(state.iterations()));
  }
}

#ifdef HAVE_BENCH_DROGON
void BenchDrogonRandom(benchmark::State &state) {
  DrogonServerWrapper server;
  PersistentClient client(server.port());
  std::size_t minSize = static_cast<std::size_t>(state.range(0));
  std::size_t maxSize = static_cast<std::size_t>(state.range(1));
  std::mt19937_64 rng{987654321ULL};
  std::uniform_int_distribution<std::size_t> dist(minSize, maxSize);
  std::size_t totalBytes = 0;
  for (auto iterationTag : state) {
    (void)iterationTag;
    std::size_t sz = dist(rng);
    std::size_t respLen = 0;
    if (!client.getSize(sz, respLen)) {
      state.SkipWithError("drogon request failed");
      break;
    }
    totalBytes += respLen;
  }
  state.SetBytesProcessed(static_cast<int64_t>(totalBytes));
  if (state.iterations() > 0) {
    state.counters["avg_bytes_per_iter"] =
        benchmark::Counter(static_cast<double>(totalBytes) / static_cast<double>(state.iterations()));
  }
}
#endif

#ifdef HAVE_BENCH_OATPP
void BenchOatppRandom(benchmark::State &state) {
  OatppServerWrapper server;
  PersistentClient client(server.port());
  std::size_t minSize = static_cast<std::size_t>(state.range(0));
  std::size_t maxSize = static_cast<std::size_t>(state.range(1));
  std::mt19937_64 rng{555555555ULL};
  std::uniform_int_distribution<std::size_t> dist(minSize, maxSize);
  std::size_t totalBytes = 0;
  for (auto iterationTag : state) {
    (void)iterationTag;
    std::size_t sz = dist(rng);
    std::size_t respLen = 0;
    if (!client.getSize(sz, respLen)) {
      state.SkipWithError("oatpp request failed");
      break;
    }
    totalBytes += respLen;
  }
  state.SetBytesProcessed(static_cast<int64_t>(totalBytes));
  if (state.iterations() > 0) {
    state.counters["avg_bytes_per_iter"] =
        benchmark::Counter(static_cast<double>(totalBytes) / static_cast<double>(state.iterations()));
  }
}
#endif

// Register argument pairs (min,max)
BENCHMARK(BenchAeronetRandom)->Args({16, 512})->Args({16, 4096})->Args({64, 8192});
#ifdef HAVE_BENCH_DROGON
BENCHMARK(BenchDrogonRandom)->Args({16, 512})->Args({16, 4096});
#endif
#ifdef HAVE_BENCH_OATPP
BENCHMARK(BenchOatppRandom)->Args({16, 512})->Args({16, 4096});
#endif

}  // namespace

BENCHMARK_MAIN();
