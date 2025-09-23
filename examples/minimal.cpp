#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"

namespace {
std::atomic_bool gStop{false};

void handleSigint([[maybe_unused]] int signum) { gStop.store(true); }
}  // namespace

int main(int argc, char **argv) {
  uint16_t port = 0;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }

  aeronet::HttpServer server(aeronet::ServerConfig{}.withPort(port));
  server.setHandler([](const aeronet::HttpRequest &req) {
    aeronet::HttpResponse resp;
    resp.body = std::string("Hello from aeronet minimal server! You requested ") + std::string(req.target) + '\n';
    resp.body += "Method: " + std::string(req.method) + "\n";
    resp.body += "Version: " + std::string(req.version) + "\n";
    resp.body += "Headers:\n";
    for (const auto &[headerKey, headerValue] : req.headers) {
      resp.body += std::string(headerKey) + ": " + std::string(headerValue) + "\n";
    }
    return resp;
  });

  std::signal(SIGINT, handleSigint);

  server.runUntil([]() { return gStop.load(); });
}
