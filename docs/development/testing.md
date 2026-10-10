# Testing

aeronet's tests use GoogleTest and run with CTest. Unit tests live next to their module, in `aeronet/<module>/test/`, and cross-module integration tests, which start real servers on loopback sockets, live in `tests/`. Shared helpers (test clients, temporary files, TLS certificates, fault injection) are in `aeronet/test_support/`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DAERONET_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure --timeout 300
```

## Network fault injection

Real networks deliver partial reads and writes, return `EAGAIN` at arbitrary points, and reset connections. Loopback tests rarely exercise these paths, so aeronet provides deterministic fault injection at the transport layer.

The hooks are compiled when `AERONET_ENABLE_TEST_HOOKS` is `ON`, which is the default for test builds that are not `Release`. They cost nothing when disabled.

| Component | Header | Role |
| --- | --- | --- |
| `FaultPolicy` | `aeronet/fault-policy.hpp` | Describes the faults: maximum bytes per read or write, periodic `EAGAIN` every N reads or writes, connection reset after a byte count or on the next operation, and an optional seed to randomize partial sizes. |
| `TestPipe` | `aeronet/test-pipe.hpp` | In-memory bidirectional byte channel, for tests without sockets or event loop. |
| `TestTransport` | `aeronet/test-transport.hpp` | In-memory `Transport` over a `TestPipe`, with a `FaultPolicy` applied, for unit tests of protocol handlers. |
| `FaultInjectingTransport` | `aeronet/fault-injecting-transport.hpp` | Decorator applying a `FaultPolicy` to a real transport, such as a plain socket. |
| `ScopedTransportDecorator` | `aeronet/transport-test-hook.hpp` | Installs a decorator that the server applies to every transport it accepts, for integration tests with real sockets and the full event loop. |

Unit tests of the components are in [test-transport_test.cpp](../../aeronet/sys/test/test-transport_test.cpp). Integration tests in [tests/network-fault-injection_test.cpp](../../tests/network-fault-injection_test.cpp) exercise partial reads and writes, simulated `EAGAIN`, connection resets, combined faults, TLS read-ahead, and pipelining under faults, against the full server.

Simulated `EAGAIN` on reads does not combine with edge-triggered `epoll` in integration tests: when the transport reports no data although the socket still holds some, no new edge arrives to wake the server. Use it in unit tests without event loop, or simulate `EAGAIN` on writes, which works in both.
