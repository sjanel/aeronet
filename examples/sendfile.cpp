#include <aeronet/aeronet.hpp>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "file.hpp"

namespace {
bool g_stop = false;
void sigint_handler([[maybe_unused]] int signum) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 0;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }

  std::string path;
  if (argc > 2) {
    path = argv[2];
  } else {
    // create a small temporary file in /tmp
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "aeronet-sendfile-example.txt";
    std::ofstream ofs(tmp);
    ofs << "This is a sendfile example file.\n";
    ofs << "You can pass a path as the second argument to use your own file.\n";
    ofs.close();
    path = tmp.string();
  }

  aeronet::HttpServer srv(aeronet::HttpServerConfig{}.withPort(port));

  // Fixed response (HttpResponse::file) on /static
  srv.router().setPath(aeronet::http::Method::GET, "/static", [path = path](const aeronet::HttpRequest& /*req*/) {
    return aeronet::HttpResponse(aeronet::http::StatusCodeOK).file(aeronet::File(path));
  });

  // Streaming response using HttpResponseWriter::file on /stream
  srv.router().setPath(aeronet::http::Method::GET, "/stream",
                       [path = path](const aeronet::HttpRequest& /*req*/, aeronet::HttpResponseWriter& writer) {
                         writer.status(aeronet::http::StatusCodeOK);
                         writer.file(aeronet::File(path), "text/plain");
                         writer.end();
                       });

  std::signal(SIGINT, sigint_handler);
  std::cout << "Serving on port " << srv.port() << " - GET /static or /stream to fetch file: " << path << "\n";
  srv.runUntil([]() { return g_stop; });
  return 0;
}
