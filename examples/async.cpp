#include <aeronet/async-http-server.hpp>
#include <aeronet/http-server-config.hpp>
#include <aeronet/http-server.hpp>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace aeronet;

int main() {
  HttpServerConfig cfg;  // ephemeral port
  cfg.withPort(0);
  HttpServer server(cfg);
  server.setHandler([](const HttpRequest&) {
    HttpResponse resp{200, "OK"};
    resp.contentType = "text/plain";
    resp.body = "hello from async server\n";
    return resp;
  });

  AsyncHttpServer async(std::move(server));
  async.start();
  std::printf("Async server listening on port %u\n", async.server().port());
  std::puts("Sleeping for 2 seconds while serving...");
  std::this_thread::sleep_for(std::chrono::seconds(2));

  async.requestStop();
  async.stopAndJoin();
  async.rethrowIfError();
  std::puts("Server stopped.");
}
