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

## Continue from here

- Add request and response policy with [Middleware and responses](middleware-and-responses.md).
- Handle compressed, chunked, or multipart bodies in [Bodies, streaming, and static files](bodies-streaming-and-files.md).
- Consult the complete [HTTP/1.1 feature matrix](../FEATURES.md#http11-feature-matrix) when checking a protocol behavior.
