#include <aeronet/aeronet.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

using namespace aeronet;

int main(int argc, char** argv) {
  uint16_t port = 0;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }

  SignalHandler::Enable();

  try {
    // Create server with router
    Router router;
    router.setDefault(
        [](const HttpRequest&) { return HttpResponse("Hello from SingleHttpServer with AsyncHandle!\n"); });

    SingleHttpServer server(HttpServerConfig{}.withPort(port), std::move(router));

    // Start server in background (non-blocking) - returns AsyncHandle
    auto handle = server.startDetached();

    std::cout << "Server listening on port " << server.port() << '\n';
    std::cout << "Server running in background...\n";
    std::cout << "Sleeping for 5 seconds while serving...\n";

    std::this_thread::sleep_for(std::chrono::seconds(5));

    // Stop the server explicitly (or let handle destructor do it automatically)
    handle.stop();

    // Check for any errors that occurred in the background thread
    handle.rethrowIfError();
    std::cout << "Server stopped cleanly.\n";
  } catch (const std::exception& e) {
    std::cerr << "Server encountered error: " << e.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
