#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"

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
  server.setHandler([](const aeronet::HttpRequest &req) {
    aeronet::HttpResponse resp;
    std::string body("Hello from aeronet minimal server! You requested ");
    body += req.target;
    body.push_back('\n');
    body += "Method: " + std::string(req.method) + "\n";
    body += "Version: " + std::string(req.version) + "\n";
    body += "Headers:\n";
    for (const auto &[headerKey, headerValue] : req.headers) {
      body += std::string(headerKey) + ": " + std::string(headerValue) + "\n";
    }
    resp.body(std::move(body));
    return resp;
  });

  std::signal(SIGINT, handleSigint);

  server.runUntil([]() { return gStop.load(); });
}
