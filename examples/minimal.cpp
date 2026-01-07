#include <aeronet/aeronet.hpp>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string_view>
#include <system_error>
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
      resp.bodyAppend("Hello from aeronet minimal server! You requested ");
      resp.bodyAppend(req.path());
      resp.bodyAppend("\nMethod: ");
      resp.bodyAppend(http::MethodToStr(req.method()));
      resp.bodyAppend("\nVersion: ");
      resp.bodyAppend(std::string_view(req.version().str()));
      resp.bodyAppend("\nHeaders:\n");
      for (const auto &[headerKey, headerValue] : req.headers()) {
        resp.bodyAppend(headerKey);
        resp.bodyAppend(": ");
        resp.bodyAppend(headerValue);
        resp.bodyAppend("\n");
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
