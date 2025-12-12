#include <aeronet/aeronet.hpp>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <utility>

using namespace aeronet;

int main(int argc, char **argv) {
  uint16_t port = 0;
  if (argc > 1) {
    const auto [ptr, errc] = std::from_chars(argv[1], argv[1] + std::strlen(argv[1]), port);
    if (errc != std::errc{} || ptr != argv[1] + std::strlen(argv[1])) {
      std::cerr << "Invalid port number: " << argv[1] << "\n";
      return EXIT_FAILURE;
    }
  }

  // Enable signal handler for graceful shutdown on Ctrl+C
  SignalHandler::Enable();

  Router router;

  try {
    router.setDefault([](const HttpRequest &req) {
      HttpResponse resp(200);
      resp.appendBody("Hello from aeronet minimal server! You requested ");
      resp.appendBody(req.path());
      resp.appendBody("\nMethod: ");
      resp.appendBody(http::MethodToStr(req.method()));
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

    SingleHttpServer server(HttpServerConfig{}.withPort(port), std::move(router));
    server.run();  // blocking run, until Ctrl+C
  } catch (const std::exception &e) {
    std::cerr << "Server encountered error: " << e.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
