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

  SignalHandler::Enable();

  Router router;
  try {
    router.setDefault([](const HttpRequest& req) {
      HttpResponse resp(200);
      resp.bodyAppend("multi reactor response ");
      resp.bodyAppend(req.path());
      resp.bodyAppend("\n");
      return resp;
    });

    MultiHttpServer multi(HttpServerConfig{}.withPort(port).withNbThreads(static_cast<uint32_t>(threads)),
                          std::move(router));

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
