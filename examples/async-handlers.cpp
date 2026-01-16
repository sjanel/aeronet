#include <aeronet/aeronet.hpp>
#include <coroutine>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

using namespace aeronet;

// A simple awaitable that suspends and immediately resumes, just to demonstrate co_await mechanics.
// In a real application, this would be an async I/O operation, a database query, or a timer.
struct Yield {
  static bool await_ready() noexcept { return false; }
  static void await_suspend(std::coroutine_handle<> handle) noexcept { handle.resume(); }
  static void await_resume() noexcept {}
};

int main(int argc, char** argv) {
  uint16_t port = 0;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }

  SignalHandler::Enable();

  Router router;

  // 1. Simple async handler returning a task
  router.setPath(http::Method::GET, "/async", [](HttpRequest&) -> RequestTask<HttpResponse> {
    // Simulate an async suspension point
    co_await Yield{};
    co_return HttpResponse("Hello from async world!\n");
  });

  // 2. Async handler reading the body asynchronously
  router.setPath(http::Method::POST, "/echo-async", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    // Asynchronously wait for the full body
    std::string_view body = co_await req.bodyAwaitable();

    co_return HttpResponse(body.size(), 200).header("X-Echo-Type", "Async").body(body);
  });

  // 3. Async handler with path parameters
  router.setPath(http::Method::GET, "/users/{id}", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    auto id = req.pathParams().find("id")->second;
    co_await Yield{};

    std::string msg = "User ID: ";
    msg += id;
    msg += "\n";

    co_return HttpResponse(msg);
  });

  SingleHttpServer server(HttpServerConfig{}.withPort(port), std::move(router));

  std::cout << "Listening on port " << server.port() << "\n";
  std::cout << "Try:\n";
  std::cout << "  curl http://localhost:" << server.port() << "/async\n";
  std::cout << "  curl -d 'hello' http://localhost:" << server.port() << "/echo-async\n";
  std::cout << "  curl http://localhost:" << server.port() << "/users/42\n";

  server.run();
  return 0;
}
