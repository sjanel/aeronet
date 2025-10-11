#include <aeronet/aeronet.hpp>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace {
std::atomic_bool gStop{false};

void handleSigint([[maybe_unused]] int signum) { gStop.store(true); }
}  // namespace

int main(int argc, char **argv) {
  uint16_t port = 0;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }

  aeronet::HttpServer server(aeronet::HttpServerConfig{}.withPort(port));
  server.router().setDefault([](const aeronet::HttpRequest &req) {
    aeronet::HttpResponse resp;
    std::string body("Hello from aeronet minimal server! You requested ");
    body += req.path();
    body.push_back('\n');
    body += "Method: " + std::string(aeronet::http::toMethodStr(req.method())) + "\n";
    body += "Version: ";
    body.append(std::string_view(req.version().str()));
    body.push_back('\n');
    body += "Headers:\n";
    for (const auto &[headerKey, headerValue] : req.headers()) {
      body += std::string(headerKey) + ": " + std::string(headerValue) + "\n";
    }
    resp.body(std::move(body));
    return resp;
  });

  std::signal(SIGINT, handleSigint);

  server.runUntil([]() { return gStop.load(); });
}
