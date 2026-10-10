# Routing

A `Router` maps a method and a path to a handler. Build it before creating the server, then hand it over:

```cpp
#include <utility>

Router router;
router.setPath(http::Method::GET, "/hello", [](const HttpRequestView&) { return HttpResponse(200).body("hello\n"); });
router.setPath(http::Method::GET | http::Method::POST, "/echo", [](const HttpRequestView& req) {
  return HttpResponse(200).body(req.body());
});

SingleHttpServer server(HttpServerConfig{}.withPort(8080), std::move(router));
server.run();
```

## Handlers

A route takes one of three kinds of handlers, all registered with `setPath()`:

| Handler | Signature | Use it when |
| --- | --- | --- |
| Synchronous | `HttpResponse(const HttpRequestView&)` | The response is computed quickly, without blocking. The common case. |
| Streaming | `void(const HttpRequestView&, HttpResponseWriter&)` | The body is too large to build in memory, and is written piece by piece. See [Streaming responses](streaming-responses.md). |
| Async | `RequestTask<HttpResponse>(HttpRequestView&)` | The handler waits for a database, another service, or any blocking call. See [Async handlers](async-handlers.md). |

All handlers of a server run on its event-loop thread: a handler that blocks delays every other connection of that thread.

## Registering routes

- `setPath(methods, path, handler)` registers a handler for one method or a set of methods, combined with `|`. Each method of a path has its own handler: registering `POST` on a path that has a `GET` handler keeps both, and registering the same method again replaces its handler.
- `setDefault(handler)` registers a fallback for requests that match no path. A streaming fallback can be registered next to a synchronous one; see [Dispatch precedence](#dispatch-precedence).
- A request whose path matches no route, without fallback, receives `404 Not Found`. A request whose path exists but not for its method receives `405 Method Not Allowed`, with an `Allow` header.
- `HEAD` requests use the `GET` handler when no `HEAD` handler is registered; the server sends the headers without the body.
- `OPTIONS` (CORS preflight, `OPTIONS *`), `TRACE`, and `CONNECT` are handled by the server, see [HTTP/1.1 conformance](../reference/http-conformance.md#configurable-behaviors).

`setPath()` returns the route entry, to attach per-route options (see [Per-route options](#per-route-options)).

## Path patterns

A path starts with `/` and is split into segments at each `/`. Empty segments (`//`) are not allowed. A segment is a literal, a pattern mixing literals and parameters, or a final wildcard.

| Pattern | Matches | Parameters |
| --- | --- | --- |
| `/users/{id}` | `/users/42` | `id` = `42` |
| `/users/{id}/posts/{post}` | `/users/42/posts/7` | `id` = `42`, `post` = `7` |
| `/api/v{}/search-{}` | `/api/v3/search-books` | `0` = `3`, `1` = `books` |
| `/users/{id:[0-9]+}` | `/users/42`, not `/users/me` | `id` = `42` |
| `/assets/*` | `/assets/app.js`, `/assets/css/site.css` | none |
| `/api/{{version}}/data` | `/api/{version}/data` | none |

**Parameters.** `{name}` captures part of a segment under `name`. `{}` captures it under a numeric key, `"0"`, `"1"`, and so on in order. A path uses either named or unnamed parameters, not both. A segment may mix literals and parameters (`foo{}bar`), but two parameters must be separated by a literal.

**Constraints.** `{name:pattern}` only matches values accepted by a regular expression. Simple character classes with repetitions, such as `[0-9]+`, `\d+`, or `[a-zA-Z0-9_-]{3,32}`, use a fast built-in matcher; other expressions (groups, alternation, anchors) fall back to `std::regex`. Constraints are compiled when the route is registered, and an invalid one throws `std::regex_error`. A value that fails a constraint lets matching continue with the other routes.

**Wildcard.** A final segment that is exactly `*` matches any remaining suffix, slashes included. Elsewhere, and inside a longer segment (`seg*`), `*` is a literal asterisk. The suffix matched by the wildcard is not exposed as a parameter: read it from `req.path()`.

**Literal braces.** `{{` and `}}` stand for literal `{` and `}`.

`setPath()` throws `std::invalid_argument` on an invalid pattern: a path not starting with `/`, an empty segment, an unterminated `{`, two consecutive parameters, an invalid parameter name, mixed named and unnamed parameters, or a pattern conflicting with an existing wildcard route. Registering a streaming handler for a method that already has a synchronous one, or the reverse, throws `std::logic_error`.

### Matching order

When several routes match a path, the most specific wins:

1. a literal segment, over
2. a parameter with a constraint, over
3. a parameter without constraint, over
4. the final wildcard.

`/users/me` is thus served by a `/users/me` route even when `/users/{id}` exists. When several constrained parameters accept the same segment, the first registered wins.

### Reading parameters

```cpp
#include <string_view>

Router router;
router.setPath(http::Method::GET, "/users/{id:[0-9]+}/posts/{post}", [](const HttpRequestView& req) {
  const std::string_view userId = req.pathParamValueOrEmpty("id");
  const auto postId = req.pathParamValue("post");  // std::optional, empty if absent
  return HttpResponse(200).body(userId);
});
```

`pathParams()` returns all of them as a map. Like every view of the request, parameter values point into the connection buffer and are valid until the handler returns: copy them to keep them longer (see [Request lifetime](requests.md#lifetime-of-request-data)).

## Trailing slashes

`RouterConfig::trailingSlashPolicy` decides how `/foo` and `/foo/` relate. An exact match is always served first; the policy only applies when the request differs from a registered route by a final slash. The root path `/` is never rewritten.

| Policy | Request `/foo/`, route `/foo` | Request `/foo`, route `/foo/` | Routes `/foo` and `/foo/` |
| --- | --- | --- | --- |
| `Normalize` (default) | served by `/foo` | served by `/foo/` | one canonical route: the first registration is kept |
| `Redirect` | `301` to `/foo` | `301` to `/foo/` | one canonical route: the first registration is kept |
| `Strict` | `404` | `404` | two distinct routes |

```cpp
RouterConfig routerConfig;
routerConfig.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
Router router(routerConfig);
```

Use `Strict` for APIs where the two forms mean different resources, `Redirect` to enforce canonical public URLs, and `Normalize` to accept both without duplicate registrations.

## Dispatch precedence

A path can have synchronous and streaming handlers for different methods (for instance a streaming `GET` and a synchronous `POST`), but a given method of a path has only one handler: registering a streaming handler for a method that already has a synchronous one throws `std::logic_error`. For a request, the server picks, in this order:

1. the streaming handler of the matched path and method,
2. the synchronous or async handler of the matched path and method,
3. the streaming fallback (`setDefault(StreamingHandler)`),
4. the synchronous or async fallback.

## Route groups

`router.group(prefix)` registers routes under a common prefix, with shared options:

```cpp
#include <chrono>

Router router;
auto api = router.group("/api/v1");
api.withMaxBodyBytes(64 * 1024).withTimeout(std::chrono::seconds{5});
api.addRequestMiddleware([](HttpRequestView&) { return MiddlewareResult::Continue(); });
api.setPath(http::Method::GET, "/users", [](const HttpRequestView&) { return HttpResponse(200); });   // /api/v1/users
api.setPath(http::Method::POST, "/users", [](const HttpRequestView&) { return HttpResponse(201); });  // /api/v1/users

auto admin = api.group("/admin");  // /api/v1/admin/..., inherits the options and middleware of api
admin.setPath(http::Method::DELETE, "/cache", [](const HttpRequestView&) { return HttpResponse(204); });
```

A group applies its options when a route is registered through it, so set them before registering routes. Options set on a route afterwards override the group's. A group does not own the router, which must outlive it.

## Per-route options

The entry returned by `setPath()` carries options for that route:

```cpp
#include <chrono>

Router router;
router.setPath(http::Method::POST, "/login", [](const HttpRequestView&) { return HttpResponse(204); })
    .maxBodyBytes(4096)  // a login form needs far less than the server-wide limit
    .timeout(std::chrono::seconds{5})
    .before([](HttpRequestView&) { return MiddlewareResult::Continue(); });
```

| Option | Effect |
| --- | --- |
| `maxHeaderBytes(n)`, `maxBodyBytes(n)` | Lower limits for this route. A route can only tighten the server limits (`HttpServerConfig::maxHeaderBytes` and `maxBodyBytes`): a larger value throws `std::invalid_argument`. Set the server limit to the largest value any route needs. |
| `timeout(duration)` | Deadline for the request, from the start of its head: when it expires, the server answers `408 Request Timeout` and closes the connection. Enforced for async and streaming handlers; a synchronous handler cannot be interrupted. |
| `before(middleware)`, `after(middleware)` | Middleware of this route only, see [Middleware](middleware.md). |
| `cors(policy)` | CORS policy of this route, see [CORS](cors.md). |
| `http2Enable(mode)` | Force HTTP/2 on or off for this route, regardless of the global setting. |

## Updating routes at runtime

A router passed to the server constructor is ready when the server starts, which is the simplest model. To change routes while the server runs, use `server.router()`, which returns a proxy forwarding each change to the event-loop thread, or `postRouterUpdate()` to apply several changes at once:

```cpp
SingleHttpServer server(HttpServerConfig{});
auto handle = server.startDetached();

// From any thread:
server.router().setPath(http::Method::GET, "/feature", [](const HttpRequestView&) { return HttpResponse(200); });
server.postRouterUpdate([](Router& router) {
  router.setPath(http::Method::GET, "/a", [](const HttpRequestView&) { return HttpResponse(200); });
  router.setPath(http::Method::GET, "/b", [](const HttpRequestView&) { return HttpResponse(200); });
});
```

An update is applied by the event loop at its next iteration (immediately when the server is not running). Requests that already started keep their handler: an async handler suspended during an update completes with the handler it started with.

## Tests

- Patterns, constraints, wildcard, escapes, trailing slashes, and precedence: [router_test.cpp](../../aeronet/http/test/router_test.cpp).
- End-to-end routing, `404` / `405`, and trailing slash redirects: [tests/http-routing_test.cpp](../../tests/http-routing_test.cpp).
