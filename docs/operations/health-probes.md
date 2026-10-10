# Health probes

aeronet can answer the health checks of Kubernetes and load balancers itself, without application handlers. The built-in probes are tiny `GET` endpoints that reflect the server's lifecycle: started, ready to take traffic, or draining.

## Enable the probes

```cpp
HttpServerConfig config;
config.withPort(8080).enableBuiltinProbes();
SingleHttpServer server(std::move(config));
```

| Probe | Default path | Answer |
| --- | --- | --- |
| Liveness | `/livez` | `200 OK` while the server runs. |
| Readiness | `/readyz` | `200 OK` while the server accepts new requests, `503 Service Unavailable` once it drains, with `Connection: close` so that clients stop reusing the connection. |
| Startup | `/startupz` | `503` until the server has started, then `200` like liveness. |

The bodies are short plain-text messages (`OK`, `Not Ready`), and `HEAD` requests are answered too. The probes take precedence over application `GET` handlers registered on the same paths.

Readiness is what makes rolling updates lossless: when the server starts draining (see [graceful drain](../guides/server-lifecycle.md#stopping)), `/readyz` fails, the load balancer or Kubernetes stops sending new connections, and in-flight requests complete.

## Configuration

`BuiltinProbesConfig` customizes the probes:

```cpp
BuiltinProbesConfig probes;
probes.enabled = true;
probes.withLivenessPath("/health/live").withReadinessPath("/health/ready").withStartupPath("/health/started");

HttpServerConfig config;
config.withBuiltinProbes(std::move(probes));
```

| Setting | Default | Notes |
| --- | --- | --- |
| `enabled` | `false` | `enableBuiltinProbes()` sets it. |
| `withLivenessPath()`, `withReadinessPath()`, `withStartupPath()` | `/livez`, `/readyz`, `/startupz` | Must be non-empty and start with `/`. |
| `contentType` | `TextPlainUtf8` | `Content-Type` of the answers. |
| `withDedicatedPort()` | `0` (disabled) | Serve the probes on their own port, see below. `MultiHttpServer` only. |
| `withLivenessStaleThreshold()` | 10 s | Liveness window of the dedicated listener. |

In a configuration file:

```yaml
server:
  builtinProbes:
    enabled: true
    dedicatedPort: 9091
    livenessStaleThreshold: 15s
```

## Isolate probes on a dedicated port

Inline probes share the event loops of the application. A handler that keeps a worker busy for a long time, with heavy computation or a slow blocking call, delays the probes that the same worker must answer. Under Kubernetes, a busy pod can then miss its liveness probe and be restarted. Adding worker threads does not reliably reserve one for probes: with `SO_REUSEPORT`, the kernel picks which worker accepts each connection.

With `withDedicatedPort(port)`, a `MultiHttpServer` starts an extra event loop, on its own thread, bound to `port`, that only answers the probes. Application handlers never run there, so probe latency no longer depends on application load. Point the Kubernetes probes at that port; the [Kubernetes guide](../kubernetes-examples.md#isolating-probes-on-a-dedicated-port) shows the manifest. A `SingleHttpServer` ignores `dedicatedPort`: it has no worker pool to isolate the probes from.

```cpp
#include <chrono>

BuiltinProbesConfig probes;
probes.enabled = true;
probes.withDedicatedPort(9091).withLivenessStaleThreshold(std::chrono::seconds{15});

HttpServerConfig config;
config.withPort(8080).withNbThreads(4).withReusePort().withBuiltinProbes(std::move(probes));

MultiHttpServer server(std::move(config));
// server.probePort() == 9091
```

The dedicated listener reports the state of the worker pool, not its own:

- **Readiness**: `200` while at least one worker accepts traffic, `503` once all workers drain.
- **Startup**: `200` once at least one worker runs its event loop.
- **Liveness**: each worker updates a heartbeat at every iteration of its event loop, and a worker stuck in a handler stops updating it. Liveness fails only when **every** worker's heartbeat is older than `livenessStaleThreshold`, that is when the whole pool is stuck. A worker that is idle, or busy for less than the threshold, keeps the pod live.

An idle worker refreshes its heartbeat once per poll cycle, so a healthy loop can look stale for up to `pollInterval` multiplied by the maximum poll interval factor. Keep `livenessStaleThreshold` well above both this value and your longest legitimate handler run time. The default 10 seconds is well above the default 500 ms poll interval. This matters most with one or two worker threads, where a single long handler can make the whole pool look stuck.

## Recommendations

- Keep built-in probes for what they do well: telling whether the process runs and accepts traffic. To check dependencies (databases, caches, downstream services), register your own handler on another path, and avoid making liveness depend on them: a database outage should not restart every pod.
- Give the startup probe a generous failure threshold when initialization is slow, so that liveness does not restart the pod during warm-up.
- With a dedicated port, check that the network policies and the `Service` do not expose that port publicly.

## Tests

- Probe answers, startup and readiness transitions, drain behavior, and path validation: the probe tests of [tests/http-server-lifecycle_test.cpp](../../tests/http-server-lifecycle_test.cpp).
- Dedicated listener (isolation from a blocked worker, liveness heartbeat, drain, validation): the `MultiHttpServerDedicatedProbes` suite of [tests/multi-http-server_test.cpp](../../tests/multi-http-server_test.cpp).
