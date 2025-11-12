#include <aeronet/aeronet.hpp>
#include <chrono>
#include <exception>
#include <iostream>
#include <thread>
#include <utility>

using namespace aeronet;

int main() {
  // Create server with router
  Router router;
  router.setDefault(
      [](const HttpRequest&) { return HttpResponse(200, "OK").body("Hello from HttpServer with AsyncHandle!\n"); });

  HttpServer server(HttpServerConfig{}, std::move(router));

  // Start server in background (non-blocking) - returns AsyncHandle
  auto handle = server.startDetached();

  std::cout << "Server listening on port " << server.port() << '\n';
  std::cout << "Server running in background...\n";
  std::cout << "Sleeping for 2 seconds while serving...\n";

  std::this_thread::sleep_for(std::chrono::seconds(2));

  // Stop the server explicitly (or let handle destructor do it automatically)
  handle.stop();

  // Check for any errors that occurred in the background thread
  try {
    handle.rethrowIfError();
    std::cout << "Server stopped cleanly.\n";
  } catch (const std::exception& e) {
    std::cerr << "Server encountered error: " << e.what() << '\n';
    return 1;
  }

  return 0;
}
