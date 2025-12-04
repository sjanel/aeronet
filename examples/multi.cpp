#include <aeronet/aeronet.hpp>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

using namespace aeronet;

int main(int argc, char** argv) {
  uint16_t port = 0;  // default to ephemeral port
  int threads = 4;    // default
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }
  if (argc > 2) {
    threads = std::stoi(argv[2]);
  }

  aeronet::SignalHandler::Enable();

  try {
    Router router;
    router.setDefault([](const HttpRequest& req) {
      std::string body("multi reactor response ");
      body.append(req.path());
      body.push_back('\n');
      return HttpResponse(std::move(body));
    });

    MultiHttpServer multi(HttpServerConfig{}.withPort(port).withReusePort(true), std::move(router),
                          static_cast<uint32_t>(threads));

    multi.run();
    // print stats
    const auto stats = multi.stats();
    std::cout << "Stats: \n" << stats.json_str() << '\n';
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
  return 0;
}
