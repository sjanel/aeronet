#include <aeronet/aeronet.hpp>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/file.hpp"

using namespace aeronet;

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

  SignalHandler::Enable();

  Router router;

  // Fixed response (HttpResponse::file) on /static
  router.setPath(http::Method::GET, "/static",
                 [path = path](const HttpRequest& /*req*/) { return HttpResponse().file(File(path)); });

  // Streaming response using HttpResponseWriter::file on /stream
  router.setPath(http::Method::GET, "/stream", [path = path](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.file(File(path), "text/plain");
    writer.end();
  });

  SingleHttpServer srv(HttpServerConfig{}.withPort(port), std::move(router));

  std::cout << "Serving on port " << srv.port() << " - GET /static or /stream to fetch file: " << path << "\n";
  srv.run();
  return 0;
}
