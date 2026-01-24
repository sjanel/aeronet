#include <aeronet/aeronet.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

using namespace aeronet;

namespace {

// Mock database: user store
struct User {
  int id;
  std::string name;
  std::string email;
};

std::unordered_map<int, User> users{
    {1, {1, "Alice", "alice@example.com"}},
    {2, {2, "Bob", "bob@example.com"}},
    {3, {3, "Charlie", "charlie@example.com"}},
};

// Simulates a blocking database lookup (e.g., network call to a remote DB).
// In a real application, this could be a call to PostgreSQL, Redis, or any external service.
std::optional<User> simulateDatabaseLookup(int userId) {
  // Simulate network latency
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto it = users.find(userId);
  if (it == users.end()) {
    return std::nullopt;
  }
  return it->second;
}

// Simulates a blocking database update
bool simulateDatabaseUpdate(int userId, std::string_view newEmail) {
  // Simulate network latency
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto it = users.find(userId);
  if (it == users.end()) {
    return false;
  }
  it->second.email = std::string(newEmail);
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 0;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }

  SignalHandler::Enable();

  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(404).body("Not found\n"); });

  // GET /async — minimal deferWork demonstration for CI smoke test
  router.setPath(http::Method::GET, "/async", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    std::string pathCopy{req.path()};
    std::string body = co_await req.deferWork([path = std::move(pathCopy)]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return std::string{"hello from deferWork on "} + path + "\n";
    });

    co_return HttpResponse(200).body(std::move(body));
  });

  // GET /users/{id} — async handler fetching user with deferWork()
  // Demonstrates how to run blocking work on a background thread without blocking the event loop.
  // The coroutine suspends, the work runs on a separate thread, then the coroutine resumes.
  router.setPath(http::Method::GET, "/users/{id}", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    auto idStr = req.pathParams().find("id")->second;
    int id = std::stoi(std::string(idStr));

    // co_await deferWork(): runs the lambda on a background thread, returns the result.
    // The event loop is free to handle other requests while waiting.
    std::optional<User> user = co_await req.deferWork([id]() { return simulateDatabaseLookup(id); });

    if (!user) {
      co_return HttpResponse(404).body("User not found\n");
    }

    std::string response =
        "ID: " + std::to_string(user->id) + "\nName: " + user->name + "\nEmail: " + user->email + "\n";
    co_return HttpResponse(200).body(response);
  });

  // POST /users/{id}/email — async handler updating user email
  // Demonstrates combining co_await bodyAwaitable() and co_await deferWork().
  // First, we asynchronously read the request body, then we defer the blocking DB update.
  router.setPath(http::Method::POST, "/users/{id}/email", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    auto idStr = req.pathParams().find("id")->second;
    int id = std::stoi(std::string(idStr));

    // co_await body: server resumes coroutine when body is fully received
    std::string_view newEmail = co_await req.bodyAwaitable();

    // co_await deferWork(): run the blocking DB update on a background thread
    bool updated =
        co_await req.deferWork([id, email = std::string(newEmail)]() { return simulateDatabaseUpdate(id, email); });

    if (!updated) {
      co_return HttpResponse(404).body("User not found\n");
    }

    co_return HttpResponse(200).body("Email updated successfully\n");
  });

  // GET /health — sync handler for comparison
  router.setPath(http::Method::GET, "/health", [](const HttpRequest&) { return HttpResponse(200).body("OK\n"); });

  SingleHttpServer server(HttpServerConfig{}.withPort(port), std::move(router));

  std::cout << "\n=== Async Handlers Demo (with deferWork) ===\n";
  std::cout << "Server on port " << server.port() << "\n\n";
  std::cout << "This demo shows how to use deferWork() to run blocking operations\n";
  std::cout << "(like database queries) on background threads without blocking the event loop.\n\n";
  std::cout << "Examples:\n";
  std::cout << "  # Minimal async handler used by CI smoke test:\n";
  std::cout << "  curl http://localhost:" << server.port() << "/async\n\n";
  std::cout << "  # Quick health check (sync):\n";
  std::cout << "  curl http://localhost:" << server.port() << "/health\n\n";
  std::cout << "  # Fetch user (async with deferWork - simulates 50ms DB latency):\n";
  std::cout << "  curl http://localhost:" << server.port() << "/users/1\n\n";
  std::cout << "  # Update user email (async body + deferWork):\n";
  std::cout << "  curl -X POST --data 'newemail@example.com' http://localhost:" << server.port()
            << "/users/1/email\n\n";

  server.run();
  return 0;
}
