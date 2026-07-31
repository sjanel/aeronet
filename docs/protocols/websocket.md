# WebSocket

Enable WebSocket support with `AERONET_ENABLE_WEBSOCKET=ON`, then register a WebSocket endpoint with the router. aeronet implements the RFC 6455 handshake and frame lifecycle, including text and binary messages, ping/pong, close handling, and optional per-message compression.

## Endpoint model

Register an endpoint with callbacks or a handler factory. A factory is useful when each accepted connection needs its own state and when the connection must send responses after receiving a message.

The [WebSocket reference](../FEATURES.md#websocket-rfc-6455) covers the upgrade handshake, frame types, close codes, endpoint configuration, compression, and thread-safety rules.

## Try the example

The buildable [WebSocket echo example](https://github.com/sjanel/aeronet/blob/main/examples/websocket-echo.cpp) serves a small browser client and demonstrates a stateful endpoint factory. Use it as the baseline for testing browser interoperability before adding application-specific authentication or message formats.
