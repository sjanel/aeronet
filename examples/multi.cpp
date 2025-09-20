#include <atomic>
#include <csignal>
#include <iostream>
#include <thread>
#include <vector>

#include "aeronet/server.hpp"

namespace {
std::atomic_bool gStop{false};
void handleSigint(int /*signum*/) { gStop.store(true); }
}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 8080;
  int threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2);
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }
  if (argc > 2) {
    threads = std::stoi(argv[2]);
  }

  std::vector<aeronet::HttpServer> servers;
  servers.reserve(static_cast<size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    aeronet::HttpServer srv(port);
    srv.enablePortReuse(true);
    srv.setHandler([](const aeronet::HttpRequest& req) {
      aeronet::HttpResponse resp;
      resp.body = "Threaded multi server response for " + std::string(req.target);
      return resp;
    });
    servers.emplace_back(std::move(srv));
  }

  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back([&servers, i] { servers[static_cast<size_t>(i)].run(); });
  }

  std::signal(SIGINT, handleSigint);
  std::cout << "Started " << threads << " servers on port " << port << " (SO_REUSEPORT). Press Ctrl-C to stop.\n";
  while (!gStop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  for (auto& server : servers) {
    server.stop();
  }
  for (auto& worker : workers) {
    worker.join();
  }
}
