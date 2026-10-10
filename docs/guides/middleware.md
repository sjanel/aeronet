# Middleware

Middleware applies cross-cutting policy around handlers: authentication, request tagging, headers added to every response, rate limiting. A **request middleware** runs before the handler and may answer instead of it; a **response middleware** runs after the handler and may change its response.

```cpp
#include <string_view>
#include <utility>

Router router;

// Request middleware: answer 401 instead of the handler when the token is missing.
router.addRequestMiddleware([](HttpRequestView& req) {
  if (req.headerValueOrEmpty("authorization").empty()) {
    return MiddlewareResult::ShortCircuit(HttpResponse(http::StatusCodeUnauthorized).body("authentication required"));
  }
  return MiddlewareResult::Continue();
});

// Response middleware: add a header to every response.
router.addResponseMiddleware([](const HttpRequestView&, HttpResponse& response) {
  response.headerAddLine("x-content-type-options", "nosniff");
});
```

## Scopes and order

| Scope | Registration |
| --- | --- |
| Every request | `Router::addRequestMiddleware()`, `Router::addResponseMiddleware()` |
| A group of routes | `RouteGroup::addRequestMiddleware()`, `RouteGroup::addResponseMiddleware()`, see [Route groups](routing.md#route-groups) |
| One route | `.before(middleware)` and `.after(middleware)` on the entry returned by `setPath()` |

For a request, the chains run in this order: global request middleware, route request middleware, handler, route response middleware, global response middleware. Within a chain, middleware runs in registration order. When no route matches (a `404`, a `405`, or a trailing slash redirect), only the global chains run.

## Request middleware

A request middleware has the signature `MiddlewareResult(HttpRequestView&)`. It returns:

- `MiddlewareResult::Continue()` to pass the request on, to the next middleware and eventually the handler;
- `MiddlewareResult::ShortCircuit(response)` to answer with `response`: the remaining request middleware and the handler are skipped, but the response still goes through the response middleware, so that headers, logging, and metrics apply uniformly.

An exception escaping a request middleware is logged and answered with `500 Internal Server Error`.

Request middleware runs before the body of an [async handler](async-handlers.md#how-a-request-flows) is received: it can check the head of the request, and answer early, but must not rely on the body. When it answers before the body arrived, the server closes the connection after the response, since the rest of the body cannot be skipped reliably.

## Response middleware

A response middleware has the signature `void(const HttpRequestView&, HttpResponse&)`: it can change the status, headers, and body. An exception escaping it is logged, and the remaining response middleware still runs.

For [streaming handlers](streaming-responses.md), response middleware runs just before the headers are sent, with the first body bytes: it can change the status and the headers, but not the body, which is written by the handler afterwards. Responses produced before the handler, such as a short-circuit, a CORS denial, or a `406` for an unacceptable content coding, go through the response middleware too.

Middleware runs on the event-loop thread, like handlers: keep it fast and non-blocking.

## Rate limiting

`RateLimitRequestMiddlewareBuilder` builds a request middleware that enforces a request rate per client, and answers `429 Too Many Requests` with a `Retry-After` header when a client exceeds it:

```cpp
#include <aeronet/rate-limit-middleware.hpp>

Router router;
RateLimitRequestMiddlewareBuilder limiter;
limiter.config.requestsPerSecond = 50;  // sustained rate
limiter.config.burst = 100;             // short bursts allowed above it
limiter.keyStrategy = RateLimitClientKeyStrategy::PeerAddress;
router.addRequestMiddleware(std::move(limiter).build());

// Or only for a group of routes:
auto api = router.group("/api");
api.addRequestMiddleware(RateLimitRequestMiddlewareBuilder{}.build());
```

| `RateLimitConfig` | Default | Meaning |
| --- | --- | --- |
| `requestsPerSecond` | 10 | Sustained rate per client. |
| `burst` | 10 | Requests a client may send at once, at least `requestsPerSecond`. |
| `maxKeys`, `idleTtl` | 65,536, 300 s | Bound on the number of tracked clients; idle clients are forgotten. |
| `failOpen` | `true` | When the store fails, let requests through rather than reject them. |
| `nbShards` | 64 | Lock shards of the in-memory store. |

The client key is chosen by `keyStrategy`:

| `RateLimitClientKeyStrategy` | Key |
| --- | --- |
| `PeerAddress` (default) | Address of the connection's peer. Behind a proxy, all clients share the proxy's address. |
| `XForwardedForFirst` | First address of `X-Forwarded-For`. |
| `HeaderValue` | Value of the header named by `headerName`, such as an API key. |
| `Custom` | Value returned by `customKeyExtractor(request)`. |

!!! warning
    Clients control `X-Forwarded-For` and other request headers. Use `XForwardedForFirst` or `HeaderValue` only behind a proxy that overwrites the header; otherwise, a client escapes the limit by changing the header on each request.

By default, the middleware keeps token buckets in memory, sharded and locked: the copies of a router made for the threads of a `MultiHttpServer` share one store, so the limit applies per process. For a limit shared by several processes, set `store` to a `RedisSlidingWindowRateLimitStore`. aeronet links no Redis client: you provide a callback that runs the store's Lua script (`luaSlidingWindowScript()`) with the client of your choice, and returns its result. The callback is called synchronously, on the event-loop thread, for every request: use a low-latency Redis and a persistent connection, and keep `failOpen` in mind for outages.

## Middleware metrics

`setMiddlewareMetricsCallback()`, on `SingleHttpServer` and `MultiHttpServer`, installs a callback receiving a `MiddlewareMetrics` record for every middleware call: phase (`Pre` or `Post`), global or route chain, index in the chain, whether it short-circuited or threw, whether the route streams, the method, and the path. Without callback, the server skips the measurement entirely.

## Tests

- Ordering, short-circuits, streaming responses, and metrics: [tests/http-routing_test.cpp](../../tests/http-routing_test.cpp).
- Rate limiting stores, refill, eviction, and concurrency: [rate-limit_test.cpp](../../aeronet/server/test/rate-limit_test.cpp) and [rate-limit-middleware_test.cpp](../../aeronet/server/test/rate-limit-middleware_test.cpp).
