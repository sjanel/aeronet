# Middleware and responses

aeronet supports ordinary request handlers, streaming handlers, and middleware that applies policy around them. Keep the handler model that matches how much output you need to retain before writing it.

## Choose a response model

- Return `HttpResponse` when the status, headers, and body are ready together.
- Use `HttpResponseWriter` for a response produced incrementally. It handles HTTP/1.1 chunk framing and works with the streaming lifecycle.
- Use asynchronous handlers when application work must suspend without blocking the event-loop thread.

The [streaming response guide](../FEATURES.md#streaming-responses-chunked--incremental) documents the writer lifecycle, trailers, and mixed streaming/non-streaming route precedence.

## Add policy with middleware

Middleware can apply cross-cutting behavior such as authentication, headers, request accounting, rate limits, CORS, and compression policy. Apply it at the narrowest useful scope so route-specific behavior remains easy to reason about.

Use the detailed references for the behavior that must be explicit in production:

- [Middleware pipeline](../FEATURES.md#middleware-pipeline)
- [Rate limiting middleware](../FEATURES.md#rate-limiting-middleware)
- [CORS helpers](../FEATURES.md#access-control-cors-helpers)
- [Compression negotiation](../FEATURES.md#compression--negotiation)
- [Reserved and managed response headers](../FEATURES.md#reserved--managed-response-headers)

## Error and connection behavior

The protocol layer handles framing and standard parser errors. Application handlers should make their own domain failures clear with an appropriate status and response body. For shutdown and client-disconnect behavior, read [connection close semantics](../FEATURES.md#connection-close-semantics) and the [server lifecycle](../FEATURES.md#httpserver-lifecycle).
