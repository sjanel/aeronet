#include <aeronet/aeronet.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

using namespace aeronet;

int main() {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200, "OK").body("hello from async server\n"); });
  AsyncHttpServer async(HttpServerConfig{}, std::move(router));
  async.start();
  std::cout << "Async server listening on port " << async.port() << '\n';
  std::cout << "Sleeping for 2 seconds while serving..." << '\n';
  std::this_thread::sleep_for(std::chrono::seconds(2));

  async.stop();
  async.rethrowIfError();
  std::cout << "Server stopped." << '\n';
}
