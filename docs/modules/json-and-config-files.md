# JSON, YAML, and configuration files

With the `AERONET_ENABLE_GLAZE` CMake option (`ON` by default when aeronet is the top-level project), aeronet integrates [Glaze](https://github.com/stephenberry/glaze) to:

- build a server or an `HttpClient` from a JSON or YAML configuration file, and dump their configuration back,
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
  trailingSlashPolicy: normalize
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

The [`aeronet-config-dump`](../../examples/config-dump.cpp) example program prints the complete default configuration, every field included, as JSON or YAML: a good starting point for a configuration file or a Kubernetes ConfigMap (see the [Kubernetes guide](../kubernetes-examples.md)). With `--client`, it prints the default client configuration instead.

### Client configuration files

A client configuration file holds a `client` section, which maps to `HttpClientConfig`, with the same rules as the server one: every field is optional, an unknown field is an error, durations are human-readable, and the format follows the file extension.

```yaml
client:
  connectTimeout: 5s
  requestTimeout: 20s
  maxRedirects: 2
  httpVersion: http1.1
  retry:
    maxAttempts: 3
    retryStatuses: [502, 503, 504]
  cache:
    refreshPeriod: 30s
    methods: [GET, HEAD]
  tlsCaFile: /etc/ssl/internal-ca.pem
  proxyUrl: http://proxy.internal:3128
```

`HttpClient` accepts the path of such a file, and `LoadClientConfig()` from `<aeronet/client-config-loader.hpp>` loads the configuration without building a client. `dumpConfig(ConfigFormat)` and `saveConfig(path)` dump the configuration of a client, as for a server:

```cpp
#include <aeronet/client-config-loader.hpp>
#include <aeronet/http-client.hpp>

#include <filesystem>
#include <string>

HttpClient client(std::filesystem::path("/etc/aeronet/client.yaml"));
const std::string yaml = client.dumpConfig(ConfigFormat::yaml);

const HttpClientConfig fromString = LoadClientConfig(R"({"client":{"maxRedirects":2}})", ConfigFormat::json);
```

Loading throws `std::runtime_error` when the file cannot be read or parsed, and the loaded configuration is validated (`HttpClientConfig::validate()`), which throws `std::invalid_argument` on inconsistent values. The dump of a client includes the `accept-encoding` header that it adds to `globalHeaders` at construction, from `defaultAcceptEncoding` or from the decoders compiled in (see [Compression and caching](../reference/client-configuration.md#compression-and-caching)): a client built from the dump sends the same header.

### Field names

A configuration file uses the names of the C++ fields: `maxRedirects`, `retry.maxAttempts`, `tls.handshakeTimeout`... The settings that are not plain fields use the name of their accessor: `certFile`, `keyPem`, `cipherList` (TLS), `livenessPath` (probes), `endpoint`, `serviceName` (telemetry), `defaultAcceptEncoding`, `proxyUrl`, `proxyCaFile`, `tlsCaFile`, `tlsClientCertFile`, `tlsClientKeyPem` (client), and so on. Enumerations are written in lower camel case (`enabledPlainOnly`, `opportunistic`), `HttpVersionMode` as `auto`, `http1.1` or `http2`, and method sets (`cache.methods` of the client, CORS `allowedMethods`) as lists of method names, which are case-sensitive.

The configuration objects are aggregates that Glaze reflects member by member, so that a field added to one of them is read and written without any serialization code. Only the members that hold internal storage (prefixed with `_`, such as the packed strings of `TLSConfig`) are mapped by hand, to the keys listed above. The callbacks are code, not configuration, and are not serializable: the TLS revocation callback and its context, and the `contentTypeResolver` and `directoryIndexRenderer` of `StaticFileConfig`. Neither are the static TLS session ticket keys (`withTlsSessionTicketKey()`).

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
- Client configuration loading and dumping, client file-path constructor: [client-config-loader_test.cpp](../../aeronet/client/test/client-config-loader_test.cpp).
- Every field reads back what is written: `EveryScalarFieldRoundTrips` changes every boolean and number of the serialized default configuration and checks that it reads back identically, so it covers new fields without any test update; `NonDefaultConfigRoundTrips` does the same for strings, lists, enumerations, and durations ([server-config-loader_test.cpp](../../aeronet/server/test/server-config-loader_test.cpp), [client-config-loader_test.cpp](../../aeronet/client/test/client-config-loader_test.cpp), [static-file-config_test.cpp](../../aeronet/objects/test/static-file-config_test.cpp)).
- Body parsing: [http-request-view_test.cpp](../../aeronet/http/test/http-request-view_test.cpp).
