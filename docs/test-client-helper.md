# Test HTTP Client Helper

The test suite consolidates repeated socket logic into small utilities to improve reliability and reduce duplication.

## Raw GET Helper (`rawGet`)

Located in `tests/test_raw_get.hpp` it performs a one-shot GET request against a host/port and fills a response struct while using ASSERT semantics for all syscalls (connect/send/recv). It automatically:

- Builds a minimal HTTP/1.1 request with Host header
- Reads until connection close or response end heuristics (Content-Length / chunked decoded)
- De-chunks bodies for convenience (tests assert on logical body)

Usage pattern in tests:

```cpp
RawGetResponse resp;
rawGet(server.port(), "/hello", resp);
EXPECT_EQ(resp.status, 200);
EXPECT_EQ(resp.body, "world");
```

## Streaming Incremental Helpers

Streaming-related tests (e.g. `http_streaming.cpp`, `http_streaming_mixed.cpp`) rely on a custom incremental reader that:

- Sends a request
- Reads partial chunks as they arrive
- Exposes concatenated body without chunk framing
- Applies a safety cap to accumulated bytes to avoid runaway memory if a bug loops

Keep-alive variants allow sending two sequential requests over the same connection to assert correct pipelining / reuse:

```cpp
HttpClientConnection c(port);
HttpResponsePieces r1 = c.request("/path1");
HttpResponsePieces r2 = c.request("/path2");
```

## Design Goals

1. Deterministic failures: every syscall is ASSERT-checked, preventing silent partial writes/reads.
2. Readability: test cases focus on HTTP semantics rather than socket ceremony.
3. Safety: size caps and timeouts prevent hangs or memory blowups in CI.

## Adding New Tests

- Prefer using existing helpers before writing raw socket code.
- If a new HTTP behavior requires custom framing (e.g. future trailers), extend the helper in a single place.
- Keep helpers header-only to ease inclusion across test translation units.

## Future Enhancements

- Possible refactor into a mini library under `tests/support/` if complexity grows.
- Additional helpers for POST bodies and chunked request generation.
