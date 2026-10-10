# CORS

Browsers block a web page from reading responses of another origin (scheme, host, and port) unless the server allows it with Cross-Origin Resource Sharing headers. aeronet implements CORS with a `CorsPolicy` attached to routes: the server answers preflight requests itself and adds the CORS headers to responses, so handlers stay unaware of it.

```cpp
#include <aeronet/cors-policy.hpp>

#include <chrono>
#include <utility>

CorsPolicy apiCors;
apiCors.allowOrigin("https://app.example.com")
    .allowOrigin("https://admin.example.com")
    .allowMethods(http::Method::GET | http::Method::POST | http::Method::PUT | http::Method::DELETE)
    .allowRequestHeader("authorization")
    .allowRequestHeader("content-type")
    .exposeHeader("x-total-count")
    .allowCredentials(true)
    .maxAge(std::chrono::hours{1});

Router router;
router.setPath(http::Method::GET | http::Method::POST, "/api/orders", [](const HttpRequestView& req) {
  return req.makeResponse(http::StatusCodeOK);
}).cors(std::move(apiCors));
```

## Attaching policies

| Scope | API |
| --- | --- |
| One route | `.cors(policy)` on the entry returned by `setPath()` |
| A group of routes | `RouteGroup::withCors(policy)`, see [Route groups](routing.md#route-groups) |
| Every route without its own policy | `RouterConfig::withDefaultCorsPolicy(policy)` |

A route policy always takes precedence over the default one. Routes without any policy send no CORS headers, so browsers block cross-origin reads of them.

## Configuration

A default-constructed `CorsPolicy` is inactive; calling any setter activates it, as does constructing it with `CorsPolicy(CorsPolicy::Active::On)`. An active policy without other setting allows any origin, without credentials, for `GET`, `HEAD`, and `POST`.

| Setter | Effect |
| --- | --- |
| `allowOrigin(origin)` | Add an allowed origin, compared case-insensitively. Repeat for several. |
| `allowAnyOrigin()` | Allow every origin (`Access-Control-Allow-Origin: *`). |
| `allowCredentials(bool)` | Allow cookies and HTTP authentication (`Access-Control-Allow-Credentials: true`). The request origin is then echoed instead of `*`, as browsers require. |
| `allowMethods(methods)` | Methods allowed cross-origin, `GET`, `HEAD`, and `POST` by default. A preflight only succeeds for methods both allowed by the policy and registered on the route. |
| `allowRequestHeader(name)`, `allowAnyRequestHeaders()` | Request headers that pages may send, beyond the CORS-safelisted ones. |
| `exposeHeader(name)` | Response headers that page scripts may read, beyond the CORS-safelisted ones. |
| `maxAge(duration)` | How long browsers may cache a preflight result (`Access-Control-Max-Age`). |
| `allowPrivateNetwork(bool)` | Answer Private Network Access preflights (`Access-Control-Allow-Private-Network: true`), for public pages calling a server on a private network. |

## Behavior

**Preflight requests.** A browser sends an `OPTIONS` request with `Origin` and `Access-Control-Request-Method` before a cross-origin request that is not "simple" (a `PUT`, a JSON body, an `authorization` header...). The server answers it without calling handlers:

| Preflight | Answer |
| --- | --- |
| Allowed | `204 No Content` with `Access-Control-Allow-Origin`, `-Allow-Methods`, `-Allow-Headers` (when headers were requested), `-Max-Age`, `-Allow-Credentials`, and `-Allow-Private-Network` as configured. |
| Origin not allowed, or requested headers not allowed | `403 Forbidden` |
| Method not allowed | `405 Method Not Allowed`, with an `Allow` header |

**Actual requests.** For a request carrying an allowed `Origin`, the server adds `Access-Control-Allow-Origin`, `Access-Control-Allow-Credentials`, and `Access-Control-Expose-Headers` as configured, to buffered and streaming responses alike, after the response middleware ran. A request whose `Origin` is not allowed is answered `403 Forbidden`, without calling the handler. Requests without `Origin` (same-origin requests, non-browser clients) are not affected by CORS.

**Caching.** When the response depends on the request origin (an allow-list, or credentials), the server adds `Origin` to the `Vary` header, so that caches do not serve the response of one origin to another.

!!! note
    CORS protects users' browsers, not the server: any non-browser client can call the API regardless of the policy. Authenticate and authorize requests independently of CORS. Avoid `allowAnyOrigin()` together with `allowCredentials(true)` on routes that rely on cookies: it lets every website act on behalf of a logged-in user.

## Tests

- Policy evaluation, preflights, origins, methods, and headers: [cors-policy_test.cpp](../../aeronet/http/test/cors-policy_test.cpp).
- End to end, including streaming responses and route precedence: [tests/http-routing_test.cpp](../../tests/http-routing_test.cpp) and [tests/http-core_test.cpp](../../tests/http-core_test.cpp).
