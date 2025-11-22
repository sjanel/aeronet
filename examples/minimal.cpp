#include <aeronet/aeronet.hpp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

int main(int argc, char **argv) {
  uint16_t port = 0;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }

  // Enable signal handler for graceful shutdown on Ctrl+C
  aeronet::SignalHandler::Enable();

  aeronet::Router router;

  router.setDefault([](const aeronet::HttpRequest &req) {
    aeronet::HttpResponse resp;
    std::string body("Hello from aeronet minimal server! You requested ");
    body += req.path();
    body.push_back('\n');
    body += "Method: " + std::string(aeronet::http::MethodToStr(req.method())) + "\n";
    body += "Version: ";
    body.append(std::string_view(req.version().str()));
    body.push_back('\n');
    body += "Headers:\n";
    for (const auto &[headerKey, headerValue] : req.headers()) {
      body += std::string(headerKey) + ": " + std::string(headerValue) + "\n";
    }
    resp.body(std::move(body));  // zero-copy body capture
    return resp;
  });

  aeronet::HttpServer server(aeronet::HttpServerConfig{}.withPort(port), std::move(router));

  server.run();  // blocking run, until Ctrl+C
}
