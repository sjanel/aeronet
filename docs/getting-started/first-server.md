# Your first server

This minimal program listens on port 8080 and returns a plain-text response from GET /hello.

## Build aeronet

From a repository checkout, configure and build a Release build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAERONET_BUILD_TESTS=OFF
cmake --build build --parallel
```

For package-manager and FetchContent integration, see [Installation and build](../INSTALL.md).

## Create a route

```cpp
#include <aeronet/aeronet-server.hpp>
#include <utility>

using namespace aeronet;

int main() {
  Router router;
  router.setPath(http::Method::GET, "/hello", [](const HttpRequestView&) {
    return HttpResponse(200).body("Hello from aeronet!\n");
  });

  SingleHttpServer server(HttpServerConfig{}.withPort(8080), std::move(router));
  server.run();
}
```

Compile this program with your CMake target linked to `aeronet_server`, then run it and make a request:

```bash
curl -i http://127.0.0.1:8080/hello
```

The server object owns the listening socket and its event loop. `run()` blocks the current thread; use the lifecycle APIs described in the [feature reference](../FEATURES.md#httpserver-lifecycle) when an application needs a non-blocking or restartable server.

!!! note
    The complete [server-minimal.cpp](../../examples/server-minimal.cpp) example adds argument parsing, graceful signal handling, and a more informative response.

Next, learn [Routing and requests](../guides/routing.md).
