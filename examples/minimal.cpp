#include <aeronet/aeronet.hpp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
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
    aeronet::HttpResponse resp(200);
    resp.appendBody("Hello from aeronet minimal server! You requested ");
    resp.appendBody(req.path());
    resp.appendBody("\nMethod: ");
    resp.appendBody(aeronet::http::MethodToStr(req.method()));
    resp.appendBody("\nVersion: ");
    resp.appendBody(std::string_view(req.version().str()));
    resp.appendBody("\nHeaders:\n");
    for (const auto &[headerKey, headerValue] : req.headers()) {
      resp.appendBody(headerKey);
      resp.appendBody(": ");
      resp.appendBody(headerValue);
      resp.appendBody("\n");
    }
    return resp;
  });

  aeronet::SingleHttpServer server(aeronet::HttpServerConfig{}.withPort(port), std::move(router));

  server.run();  // blocking run, until Ctrl+C
}
