# Scripted Client Benchmarks

This directory benchmarks **aeronet's `HttpClient`** against other production C++ HTTP clients
(**libcurl**, **Drogon**, **Boost.Beast**) under identical conditions, to show how aeronet compares on
throughput, latency and memory.

It is the mirror image of [`../scripted-servers`](../scripted-servers): instead of *many servers + one
external load generator*, it is **one over-provisioned server + many client implementations being measured**.

## How it works

```text
            ┌─────────────────────────────────────────────┐
            │  aeronet-bench-server --threads <nproc>      │  <- never the bottleneck
            └─────────────────────────────────────────────┘
                         ▲      ▲      ▲      ▲
                         │      │      │      │   (each driver: --threads worker threads,
        ┌────────────────┘      │      │      └───────────────┐  one client + one connection each)
   aeronet-bench-client   curl-bench-client   drogon-bench-client   beast-bench-client
```

* A single **`aeronet-bench-server`** is started with a high thread count so it can always keep up: the
  measured cost is therefore the **client's** (request serialization, response parsing, buffer/alloc
  management), not the server's.
* Every driver shares [`bench-client-harness.hpp`](bench-client-harness.hpp): a uniform CLI, a multi-threaded
  timing loop, a bounded-memory HdrHistogram-lite for latency percentiles, and a uniform JSON result line.
* **Fair concurrency model**: `--threads N` spawns `N` worker threads, each owning its own client instance and
  one keep-alive connection running a synchronous request/response loop. That is the natural mode of every
  client here (aeronet's `HttpClient` is one-instance-per-thread; libcurl easy, Drogon's synchronous
  `sendRequest`, and Beast's blocking I/O are all synchronous).
* **Apples-to-apples**: all drivers send `Accept-Encoding: identity`, so the server never compresses and we
  compare raw transfer + parsing (no codec asymmetry between clients).
* **Protocol-agnostic**: the same drivers and scenarios run over HTTP/1.1, cleartext HTTP/2 (`h2c`) and
  HTTP/2 over TLS (`h2-tls`), selected with `--protocol` (mirroring the scripted-*server* runner). For the
  HTTP/2 protocols only the HTTP/2-capable clients are measured (see [Protocols](#protocols)).

## Building

The drivers build automatically whenever the HTTP client and benchmarks are enabled (top-level builds). Use a
**Release** build for meaningful numbers (LTO is enabled there):

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
ninja -C build-release aeronet-bench-client curl-bench-client drogon-bench-client beast-bench-client aeronet-bench-server
```

Driver availability:

| Driver                 | Built when…                                           |
|------------------------|-------------------------------------------------------|
| `aeronet-bench-client` | always (requires `AERONET_ENABLE_HTTP_CLIENT`)        |
| `curl-bench-client`    | libcurl is found (`find_package(CURL)`)               |
| `drogon-bench-client`  | `AERONET_BENCH_ENABLE_DROGON` (Drogon is fetched)     |
| `beast-bench-client`   | `AERONET_BENCH_ENABLE_BEAST` (Boost headers resolved) |

## Running

The orchestrator starts the server, runs each available driver across the scenarios, prints a comparison
table and writes JSON (and optionally HTML) artifacts:

```bash
cd build-release/benchmarks/scripted-clients
./run_client_benchmarks.py                          # all clients, all scenarios (HTTP/1.1)
./run_client_benchmarks.py --client aeronet,curl    # subset of clients
./run_client_benchmarks.py --scenario small-get,large-get
./run_client_benchmarks.py --threads 8 --duration 15s --warmup 3s --html
./run_client_benchmarks.py --protocol h2c           # cleartext HTTP/2 (aeronet + curl)
./run_client_benchmarks.py --protocol h2-tls        # HTTP/2 over TLS (ALPN "h2")
```

Options:

```text
--client a,b         comma-separated client subset (default: aeronet,curl,drogon,beast)
--scenario a,b       comma-separated scenario subset
--protocol P         http1 | h2c | h2-tls (default: http1); see Protocols below
--threads N          client worker threads / connections (default: 4)
--server-threads N   aeronet-bench-server threads (default: nproc)
--duration D         measured window per run, e.g. 10s / 500ms (default: 10s)
--warmup D           warmup window per run (default: 2s)
--port N             server port (default: 8090)
--output DIR         artifact output directory (default: ./results)
--html               also write an HTML report with bar charts
--build-dir DIR      override build-dir auto-detection
```

## Protocols

The suite runs over three protocols, selected with `--protocol` exactly as the scripted-*server* runner
(`../scripted-servers/run_benchmarks.py`) does:

| `--protocol` | Transport        | Server flags              | Clients measured             |
|--------------|------------------|---------------------------|------------------------------|
| `http1`      | HTTP/1.1         | *(none)*                  | aeronet, curl, drogon, beast |
| `h2c`        | cleartext HTTP/2 | `--h2`                    | aeronet, curl                |
| `h2-tls`     | HTTP/2 over TLS  | `--h2 --tls --cert --key` | aeronet, curl                |

* **HTTP/2 uses prior knowledge**, not the deprecated HTTP/1.1 `Upgrade` dance: `h2c` opens directly with the
  HTTP/2 preface (RFC 9113 §3.4), `h2-tls` negotiates `h2` via ALPN. aeronet drives its native HTTP/2 engine
  (`HttpVersionMode::Http2`); libcurl drives nghttp2 (`CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE` / `_2TLS`).
* **Only HTTP/2-capable clients are measured** for `h2c` / `h2-tls`. **Drogon**'s synchronous client and
  **Boost.Beast** are HTTP/1.1-only, so they are skipped (a driver invoked directly with an HTTP/2
  `--protocol` it cannot speak exits with a clear message instead of silently downgrading).
* **h2-tls** reuses the scripted-server self-signed cert (`../scripted-servers/certs/`), auto-generating it
  via `setup_bench_resources.py` if missing. The clients skip certificate verification (self-signed), so the
  measured cost is the TLS record + HTTP/2 framing path, not PKI.
* **Artifacts** for `h2c` / `h2-tls` are written with a protocol suffix (`client_benchmark_latest_h2c.json`,
  `..._h2-tls.json`, matching badge + HTML) so successive protocol runs do not clobber one another. The
  default `http1` keeps the historical unsuffixed filenames.

### Running a single driver directly

Each driver is a standalone executable with the same CLI (handy for profiling one client):

```bash
# start a server in another terminal
./benchmarks/scripted-servers/aeronet-bench-server --port 8090 --threads 16

# then point a driver at it
./benchmarks/scripted-clients/aeronet-bench-client --url http://127.0.0.1:8090 \
    --scenario small-get --threads 4 --duration 10s --warmup 2s

# HTTP/2 (start the server with --h2; use --protocol h2c and a matching driver)
./benchmarks/scripted-clients/curl-bench-client --url http://127.0.0.1:8090 \
    --scenario small-get --protocol h2c --threads 4 --duration 10s --warmup 2s
```

## Scenarios

| Scenario    | Method | Endpoint                    | Stresses                                                      |
|-------------|--------|-----------------------------|---------------------------------------------------------------|
| `small-get` | GET    | `/ping` (3-byte body)       | Pure request-build + response-parse overhead (headline)       |
| `headers`   | GET    | `/headers?count=32&size=64` | 24 request headers serialized + 32 response headers parsed    |
| `large-get` | GET    | `/body?size=1MiB`           | Big response payload: body read / copy throughput (big get)   |
| `post`      | POST   | `/uppercase` (4 KiB)        | Request body write path                                       |
| `json`      | GET    | `/json?items=200`           | Mixed small-payload parse                                     |
| `compress`  | GET    | `/json?items=800` + gzip    | Automatic content decompression: server gzips, client decodes |
| `no-reuse`  | GET    | `/ping`                     | Fresh TCP connection per request (connect overhead)           |

### Fair compression comparison

In the `compress` scenario the client advertises `Accept-Encoding: gzip`, the server returns a gzip'd body,
and the client decodes it. **aeronet** decodes natively (its built-in content-coding, backed by **zlib-ng**).
To keep the codec identical across clients, the competitors do **not** use their own bundled inflate — they
decode the raw gzip body with the **same zlib-ng** (native `zng_*` API), exactly as the scripted *server*
benchmarks do. So the only thing that differs is the integration (automatic vs. an explicit decode step), not
the codec. The reported throughput/bytes are for the **decoded** payload, so every client reports the same
per-request byte count.

## Metrics

Per `(client × scenario)`:

* **rps** — requests per second (headline throughput)
* **latency** — avg / p50 / p90 / p99 / max (µs), from a per-thread histogram merged across threads
* **MB/s** — response transfer throughput
* **RSS** — peak resident set size of the client process (`getrusage(ru_maxrss)`), for the memory story

## Artifacts, HTML report and CI

Each run writes, under the output directory (`./results` by default):

* `client_benchmark_latest.json` — machine-readable summary in the **exact schema** the scripted-server
  renderer consumes (clients map onto its `servers` axis), plus a timestamped copy.
* `client_benchmark_badge.json` — a [shields.io](https://shields.io/endpoint) endpoint badge (aeronet peak rps).
* with `--html`, `client_benchmark.html` — the dashboard, generated by **reusing**
  [`../scripted-servers/render_benchmarks_html.py`](../scripted-servers/render_benchmarks_html.py) (the same
  renderer the HTTP/1.1, HTTP/2 and WebSocket dashboards use).

The `benchmarks-gh-pages` workflow runs this suite on every push/PR to `main` (and weekly), renders the page
with that same renderer, and publishes it to **GitHub Pages** at
[`/benchmarks/clients/`](https://sjanel.github.io/aeronet/benchmarks/clients/); the README "Client Benchmarks"
badge points at it.

## Tips for accurate measurement

The same advice as the scripted-server benchmarks applies: pin the performance governor, keep the server and
client on separate cores (`taskset`), warm up (handled via `--warmup`), and take the median of a few runs.
See [`../scripted-servers/README.md`](../scripted-servers/README.md#tips-for-accurate-benchmarking) for the
full CPU-pinning recipe.
