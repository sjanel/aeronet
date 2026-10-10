# JSON, YAML, and configuration files

With the `AERONET_ENABLE_GLAZE` CMake option (`ON` by default when aeronet is the top-level project), aeronet integrates [Glaze](https://github.com/stephenberry/glaze) to:

- build a server from a JSON or YAML configuration file, and dump a running server's configuration back,
- serialize response bodies and parse request bodies as JSON or YAML, directly from and into C++ types.

Glaze also enables the [JWT module](jwt.md).

## Configuration files

A configuration file holds a `server` section, which maps to `HttpServerConfig`, and an optional `router` section, which maps to `RouterConfig`. The format follows the file extension: `.json`, `.yaml`, or `.yml`.

```yaml
server:
  port: 8080
  keepAliveTimeout: 15s
  maxBodyBytes: 16777216
  builtinProbes:
    enabled: true
router:
  trailingSlashPolicy: Normalize
```

Every field is optional: a missing field keeps its default value. An unknown field is an error, so a typo does not silently fall back to a default. Durations are written in human-readable form, such as `500ms`, `15s`, or `1h30m`, and are dumped in the same form, so that a dumped file reads back identically.

Construct a server directly from the file, then register the handlers in code:

```cpp
#include <filesystem>
#include <utility>

auto hello = [](const HttpRequestView&) { return HttpResponse(200).body("hello\n"); };

SingleHttpServer server(std::filesystem::path("/etc/aeronet/server.yaml"));
server.router().setPath(http::Method::GET, "/hello", hello);
server.run();
```

Both `SingleHttpServer` and `MultiHttpServer` accept a configuration path, optionally followed by a pre-built `Router` whose routes are kept. To load only the server section, without building a server, use `LoadServerConfig()` from `<aeronet/server-config-loader.hpp>`:

```cpp
#include <aeronet/server-config-loader.hpp>

#include <filesystem>

const HttpServerConfig fromFile = LoadServerConfig(std::filesystem::path("/etc/aeronet/server.json"));
const HttpServerConfig fromString = LoadServerConfig(R"({"server":{"port":8080}})", ConfigFormat::json);
```

Loading throws `std::runtime_error` when the file cannot be read or parsed. The loaded configuration is then validated like one built in code (`HttpServerConfig::validate()` and `RouterConfig::validate()`), which throws on inconsistent values.

### Dump a configuration

`dumpConfig(ConfigFormat)` serializes the configuration of a server, router settings included, and `saveConfig(path)` writes it to a file in the format of its extension:

```cpp
SingleHttpServer server(HttpServerConfig{}.withPort(8080));
const std::string json = server.dumpConfig(ConfigFormat::json);
server.saveConfig("/tmp/aeronet-config.yaml");
```

The [`aeronet-config-dump`](../../examples/config-dump.cpp) example program prints the complete default configuration, every field included, as JSON or YAML: a good starting point for a configuration file or a Kubernetes ConfigMap (see the [Kubernetes guide](../kubernetes-examples.md)).

## JSON and YAML bodies

The body helpers live in `<aeronet/http-json.hpp>`, which includes Glaze. Glaze is expensive to compile, so the widely included core headers only declare these helpers: include `<aeronet/http-json.hpp>` in the files that use them. The `<aeronet/aeronet.hpp>` umbrella header includes it.

Any type Glaze can serialize works: standard containers, and aggregates such as `struct Order { std::string item; int quantity; };`, which Glaze reflects without annotations.

```cpp
#include <aeronet/http-json.hpp>

#include <string>
#include <unordered_map>

using Fields = std::unordered_map<std::string, std::string>;

Router router;
router.setPath(http::Method::POST, "/echo", [](const HttpRequestView& request) {
  auto fields = request.bodyAs<Fields>();  // std::expected<Fields, HttpResponse>
  if (!fields) {
    return std::move(fields.error());  // 400 Bad Request describing the parse error
  }
  (*fields)["status"] = "received";
  return HttpResponse(201).bodyJson(*fields);  // Content-Type: application/json
});
```

| Helper | Effect |
| --- | --- |
| `HttpRequestView::bodyAs<T>()` | Parse a JSON body into `T`. On failure, the error is a ready-to-send `400 Bad Request` response whose body describes the parse error. |
| `HttpRequestView::bodyAsYaml<T>()` | Same, for YAML. |
| `HttpResponse::bodyJson(obj)` | Serialize `obj` as the JSON body and set `Content-Type: application/json`. |
| `HttpResponse::bodyYaml(obj)` | Serialize `obj` as the YAML body and set `Content-Type: text/yaml`. |

Serialization writes directly into the response buffer, without an intermediate string. A serialization failure throws, which the server turns into a `500 Internal Server Error`. Request parsing does not throw: an unknown field or a type mismatch is reported in the `400` response.

## Tests

- Configuration loading, dumping, durations, validation, and `bodyJson()` / `bodyYaml()`: [server-config-loader_test.cpp](../../aeronet/server/test/server-config-loader_test.cpp).
- Body parsing: [http-request-view_test.cpp](../../aeronet/http/test/http-request-view_test.cpp).
