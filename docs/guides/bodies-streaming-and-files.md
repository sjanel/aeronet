# Bodies, streaming, and static files

Select a body path based on the size, transfer mode, and ownership requirements of your endpoint. aeronet exposes a buffered path for ordinary requests and specialized helpers for chunked, compressed, multipart, streaming, and file responses.

## Incoming request bodies

- Configure body and header limits before accepting untrusted traffic.
- Enable and configure inbound decompression only when your API accepts compressed uploads.
- Use multipart helpers for `multipart/form-data` rather than reparsing boundaries in application code.
- Read request trailers only for protocols and transfer modes that provide them.

Read the [inbound decompression details](../FEATURES.md#inbound-request-decompression-config-details), [multipart guide](../FEATURES.md#multipartform-data-utilities-rfc-7578), and [chunked transfer reference](../FEATURES.md#chunked-transfer-encoding-rfc-7230-41) for limits and edge cases.

## Outgoing streaming bodies

Use `HttpResponseWriter` when the body is generated over time or would otherwise require a large temporary allocation. Its `writeBody()` calls emit incremental HTTP/1.1 chunks; call `end()` once all output and optional trailers are written.

For the complete response model, see [Streaming responses](../FEATURES.md#streaming-responses-chunked--incremental) and [large body optimization](../FEATURES.md#large-body-optimization).

## Static files and ranges

The static-file helper supports efficient plain-socket transfer and HTTP range and conditional-request behavior. It is suitable for explicit file-serving endpoints, not as a replacement for a dedicated CDN where that is the better operational fit.

See [Static File Handler](../FEATURES.md#static-file-handler-rfc-9110-range-and-conditional-requests) and the buildable [static-file example](../../examples/static-file.cpp).
