# CONNECT tunneling

The `CONNECT` method ([RFC 9110 §9.3.6](https://www.rfc-editor.org/rfc/rfc9110#section-9.3.6)) asks the server to open a TCP connection to another host and relay bytes in both directions. It is how a client reaches an HTTPS origin through a forward proxy. aeronet supports it over HTTP/1.1, where the tunnel takes over the whole connection, and over HTTP/2, where each tunnel is one stream.

CONNECT is **disabled by default**. A server that relays connections to arbitrary hosts is an open proxy, so the allowed targets must be listed explicitly.

## Enable CONNECT

`HttpServerConfig::withConnectAllowlist()` sets the hosts a client may connect to:

```cpp
#include <array>
#include <string_view>

static constexpr std::array<std::string_view, 2> kAllowedTargets{"api.internal.example", "10.0.0.12"};

HttpServerConfig config;
config.withPort(3128).withConnectAllowlist(kAllowedTargets.begin(), kAllowedTargets.end());
```

| Allowlist | Effect |
| --- | --- |
| Empty (default) | CONNECT is refused with `403 Forbidden`. |
| Host names or IP addresses | The target host must match an entry exactly, case-insensitively. A listed host can be reached on any port. |
| `"*"` | Every host and port is allowed. |

The allowlist is also available in JSON and YAML configuration files as `connectAllowlist`.

!!! danger
    `"*"` lets clients reach loopback, private network, link-local, and cloud metadata addresses (such as `169.254.169.254`) from the server's network position. Use it only when unrestricted proxying is intended and access to the listener is controlled by other means.

## HTTP/1.1 tunnels

The request target of a CONNECT request is in authority form, `host:port`:

```http
CONNECT api.internal.example:443 HTTP/1.1
Host: api.internal.example:443
```

The server then:

1. validates the target: the port must be numeric, between 1 and 65535 (RFC 3986 `port = *DIGIT`). A missing port, a service name such as `host:https`, or an out-of-range value is answered `400 Bad Request`, before any name resolution;
2. checks the allowlist, and answers `403 Forbidden` for a target that is not allowed;
3. resolves the host and starts a non-blocking TCP connection. A resolution failure, or a connection that fails, is answered `502 Bad Gateway`;
4. answers `200 Connection Established` and relays bytes between the client and the target until either side closes.

From that point, the connection no longer carries HTTP: the server forwards bytes without parsing them. Bytes the client sends right after the CONNECT head, even in the same packet, or while the `200` response is being written, are forwarded too. Each side keeps its own buffer for the bytes forwarded to it, separate from HTTP response buffering. When one side closes or fails, the server closes the other side. Tunnels are not subject to the keep-alive idle timeout.

## CONNECT over HTTP/2

Over HTTP/2 ([RFC 9113 §8.5](https://www.rfc-editor.org/rfc/rfc9113#section-8.5)), a tunnel occupies a single stream, so several tunnels and ordinary requests can share one connection.

- The request carries `:method CONNECT` and `:authority host:port`, and neither `:scheme` nor `:path`. The port must be numeric and in range (`400` otherwise), and the allowlist applies (`403`).
- On success, the server answers `200` without ending the stream. DATA frames on the stream then carry the tunneled bytes in both directions.
- When the connection to the target fails after the request was accepted, the stream is reset with `CONNECT_ERROR`.
- `END_STREAM` or `RST_STREAM` from either side tears the tunnel down, and closing the HTTP/2 connection closes all its tunnels.

Extended CONNECT (RFC 8441, the `:protocol` pseudo-header used for WebSocket over HTTP/2) is not supported.

## Limitations

- Name resolution uses `getaddrinfo` on the event-loop thread, which blocks the loop until the resolver answers. Prefer IP addresses or names the resolver answers locally (`/etc/hosts`, a local caching resolver) in the allowlist.
- The allowlist matches hosts, not ports. Restrict ports with a firewall when needed.
- The server does not authenticate CONNECT requests (no `Proxy-Authorization` support), and HTTP/1.1 CONNECT requests are handled before request middleware runs. Restrict who can reach a listener that allows CONNECT.

## Tests

- HTTP/1.1 tunnels, target validation, allowlist, DNS failure, and cleanup: [tests/http-connect_test.cpp](../../tests/http-connect_test.cpp).
- HTTP/2 tunnels: [tests/http2-connect_test.cpp](../../tests/http2-connect_test.cpp).
