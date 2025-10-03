#include <aeronet/aeronet.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

using namespace aeronet;

int main() {
  HttpServerConfig cfg;  // ephemeral port
  cfg.withPort(0);
  HttpServer server(cfg);
  server.setHandler([](const HttpRequest&) {
    return HttpResponse(200, "OK").contentType(http::ContentTypeTextPlain).body("hello from async server\n");
  });

  AsyncHttpServer async(std::move(server));
  async.start();
  std::cout << "Async server listening on port " << async.server().port() << '\n';
  std::cout << "Sleeping for 2 seconds while serving..." << '\n';
  std::this_thread::sleep_for(std::chrono::seconds(2));

  async.requestStop();
  async.stopAndJoin();
  async.rethrowIfError();
  std::cout << "Server stopped." << '\n';
}
