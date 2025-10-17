#include <benchmark/benchmark.h>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"

namespace {
class BasicRoundTrip : public benchmark::Fixture {
 protected:
  void SetUp([[maybe_unused]] const benchmark::State& state) override {
    server_ = std::make_unique<aeronet::HttpServer>(aeronet::HttpServerConfig{}.withPort(0));
    server_->router().setDefault([](const aeronet::HttpRequest&) {
      aeronet::HttpResponse resp;
      resp.body("OK");
      return resp;
    });
  }
  void TearDown([[maybe_unused]] const benchmark::State& state) override { server_.reset(); }
  std::unique_ptr<aeronet::HttpServer> server_;
};

BENCHMARK_F(BasicRoundTrip, GET)(benchmark::State& state) {
  auto port = server_->port();
  for ([[maybe_unused]] auto iter : state) {
    aeronet::test::RequestOptions opt;  // default GET /
    auto raw = aeronet::test::request(port, opt);
    if (!raw) {
      state.SkipWithError("request failed");
      break;
    }
    benchmark::DoNotOptimize(raw->data());
  }
}
}  // namespace
