#include <benchmark/benchmark.h>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "tests/test_http_client.hpp"

namespace {
class BasicRoundTrip : public benchmark::Fixture {
 protected:
  void SetUp(const benchmark::State&) override {
    server_ = std::make_unique<aeronet::HttpServer>(aeronet::HttpServerConfig{}.withPort(0));
    server_->setHandler([](const aeronet::HttpRequest&) {
      aeronet::HttpResponse r;
      r.body = "OK";
      return r;
    });
  }
  void TearDown(const benchmark::State&) override { server_.reset(); }
  std::unique_ptr<aeronet::HttpServer> server_;
};

BENCHMARK_F(BasicRoundTrip, GET)(benchmark::State& state) {
  auto port = server_->port();
  for (auto iter : state) {
    (void)iter;
    test_http_client::RequestOptions opt;  // default GET /
    auto raw = test_http_client::request(port, opt);
    if (!raw) {
      state.SkipWithError("request failed");
      break;
    }
    benchmark::DoNotOptimize(raw->data());
  }
}
}  // namespace
