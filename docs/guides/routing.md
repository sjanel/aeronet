# Routing and requests

`Router` maps an HTTP method and path pattern to a handler. A handler receives an `HttpRequestView` and returns an `HttpResponse`; use `HttpResponseWriter` when the response must stream incrementally.

## Route design

- Use an explicit method and path for normal endpoints with `Router::setPath()`.
- Use path parameters and the documented matching rules for resource-oriented routes.
- Use a default handler only when your application deliberately needs a fallback.
- Let aeronet produce the method and path semantics it owns, including deterministic 405 Method Not Allowed handling for registered paths.

The detailed syntax, parameter decoding, trailing-slash behavior, and precedence rules are in the [routing patterns reference](../FEATURES.md#routing-patterns--path-parameters).

## Read a request safely

`HttpRequestView` exposes the path, method, headers, query parameters, body, and trailers without asking the handler to own the connection. Its views are tied to the request lifetime, so copy data only when it must outlive the handler.

For asynchronous handlers, wait for the body with the supported awaitable helpers before accessing a body that is still arriving. The [memory-management reference](../FEATURES.md#memory-management--stdstring_view-safety) explains these lifetime guarantees.

## A parameterized JSON-shaped response

The handler receives a non-owning request view, so values copied into a response may safely outlive the handler call. Keep a `string_view` only when it remains inside the request lifetime.

```cpp
Router router;
router.setPath(http::Method::GET, "/users/{userId}", [](const HttpRequestView& request) {
  const std::string_view userId = request.pathParamValueOrEmpty("userId");
  return HttpResponse(200).body(std::string{"{\"id\":\""} + std::string{userId} + "\"}", "application/json");
});
```

The exact parameter syntax and precedence are documented in [routing patterns](../FEATURES.md#routing-patterns--path-parameters). Use a default handler deliberately: registered paths retain aeronet's deterministic 404 and 405 behavior.

## Continue from here

- Add request and response policy with [Middleware and responses](middleware-and-responses.md).
- Handle compressed, chunked, or multipart bodies in [Bodies, streaming, and static files](bodies-streaming-and-files.md).
- Consult the complete [HTTP/1.1 feature matrix](../FEATURES.md#http11-feature-matrix) when checking a protocol behavior.
