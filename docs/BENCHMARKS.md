# aeronet Benchmarks

This document describes the purpose, scope, and usage of the benchmarking suite for aeronet.

## Goals

- Track performance regressions for core HTTP, HTTP/2 and WebSocket request handling, and for the HTTP client.
- Compare aeronet with other servers (drogon, pistache, Crow, Boost.Beast, uWebSockets, Rust, Undertow, Go, Python) and clients (libcurl, drogon, Beast) with the same scripted workloads.
- Keep CI benchmark runs reproducible with pinned tooling and generated artifacts.
- Offer reproducible local runs with JSON + HTML output for ad-hoc analysis.

## Non‑Goals

- Producing authoritative cross-platform numbers (cloud CI noise is high).
- Shipping benchmark binaries in packages.
- Providing full-feature load generation: `wrk`, `h2load` and the native `ws-loadgen` do that externally.

## Suites

| Suite | Location | Measures | Documentation |
| ----- | -------- | -------- | ------------- |
| Internal micro-benchmarks | `benchmarks/internal/` | One component in-process (parser, router, HPACK, transport dispatch, ...), with Google Benchmark | [Targets](#targets) |
| Scripted server benchmarks | `benchmarks/scripted-servers/` | Whole servers under `wrk` (HTTP/1.1), `h2load` (HTTP/2) and `ws-loadgen` / k6 (WebSocket) | [CI and GitHub Pages](#ci-and-github-pages), [scripted-servers README](../benchmarks/scripted-servers/README.md) |
| Scripted client benchmarks | `benchmarks/scripted-clients/` | `HttpClient` against libcurl, drogon and Beast (HTTP/1.1, h2c, h2-tls) | [scripted-clients README](../benchmarks/scripted-clients/README.md) |
| Framework comparison | `benchmarks/frameworks/` | Small in-process comparison of aeronet with drogon, oatpp and cpp-httplib | [Comparative Frameworks](#comparative-frameworks) |

## CI and GitHub Pages

Benchmark CI is executed by `.github/workflows/benchmarks-gh-pages.yml`.

It runs:

- HTTP/1.1 benchmarks (wrk)
- HTTP/2 benchmarks (h2load, h2c + h2-tls)
- WebSocket benchmarks (`ws-loadgen`, a native load generator)
- Client benchmarks (aeronet `HttpClient` vs libcurl, Drogon and Beast, `benchmarks/scripted-clients/`)

The HTTP scenarios include a gzip round trip (`/body-codec`, `benchmarks/scripted-servers/lua/body_codec.lua`) measuring automatic request decompression and response compression together.

The same workflow publishes rendered dashboards and badge endpoint JSON files to GitHub Pages:

- HTTP/1.1: `https://sjanel.github.io/aeronet/benchmarks/`
- HTTP/2 h2c: `https://sjanel.github.io/aeronet/benchmarks/h2/benchmarks_h2c.html`
- HTTP/2 TLS: `https://sjanel.github.io/aeronet/benchmarks/h2/benchmarks_h2-tls.html`
- WebSocket: `https://sjanel.github.io/aeronet/benchmarks/ws/`

For WebSocket runs, if aeronet reports benchmark check failures/errors, the benchmark CI job fails by design.

## Saturation: measure the server, or the client, not the noise

A throughput number only measures the side that is the bottleneck. When the load generator cannot keep the
server busy, a server benchmark measures the load generator (and the scheduler); when a client waits on the
server or on the kernel, a client benchmark measures them instead. Every runner therefore checks it:

- **Server benchmarks** (`run_benchmarks.py` for HTTP/1.1 and HTTP/2, `run_ws_benchmarks.py` for WebSocket)
  pin the server and the load generator to disjoint CPUs (one server CPU per physical core when possible, the
  load generator on the remaining ones, SMT siblings of the server last) and measure the server CPU utilization
  over each run (`bench_utils.CpuMeter`). The load generator gets up to 3 threads per server thread by default
  (`--loadgen-threads` overrides it).
- **Client benchmarks** (`run_client_benchmarks.py`) give each client 3 synchronous connections per reserved
  CPU, so that the CPU stays busy while connections wait for responses, and record the client CPU time over
  the measured window (reported by the driver itself) along with the server CPU utilization.

A run whose measured side used less than 90% of its CPUs is flagged as not saturated: in the console, in the
JSON summary (`results.<scenario>.server_cpu`, or `client_cpu` with `"measured_side": "client"`) and in the HTML
report (CPU table and warning banner). The CI benchmark jobs print the CPU topology (`lscpu`) of their runner.

## WebSocket Benchmarks

The WebSocket scripted benchmark harness lives under `benchmarks/scripted-servers/` and is driven by:

- `run_ws_benchmarks.py` (orchestration)
- `ws-loadgen` (`ws_loadgen.cpp`), the native epoll load generator used by default: k6 spends several times
  more CPU per message than the servers and cannot saturate them on small machines
- k6 scenario scripts under `benchmarks/scripted-servers/k6/` (with `--tool k6`)
- `render_benchmarks_html.py` (dashboard generation)

Run locally from the benchmark build directory:

```bash
./run_ws_benchmarks.py --duration 30s --vus 50 --output ./ws-results
```

Artifacts produced:

- Timestamped JSON summary: `ws_benchmark_*.json`
- Stable JSON pointer for CI/artifacts: `ws_benchmark_latest.json`
- HTML report: `ws_benchmark_*.html`
- Shields badge payload: `ws_benchmark_badge.json`

## Build Activation

Benchmarks are built by default only when aeronet is the top-level project, and not in Debug builds. Enable them explicitly with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAERONET_BUILD_BENCHMARKS=ON
cmake --build build --target run-aeronet-bench
```

When aeronet is consumed as a dependency (FetchContent, Conan, vcpkg), benchmarks are always OFF by default. The comparison frameworks are fetched or built according to the `AERONET_BENCH_ENABLE_OATPP`, `AERONET_BENCH_ENABLE_DROGON`, `AERONET_BENCH_ENABLE_HTTPLIB`, `AERONET_BENCH_ENABLE_PISTACHE`, `AERONET_BENCH_ENABLE_CROW`, `AERONET_BENCH_ENABLE_BEAST` and `AERONET_BENCH_ENABLE_UWEBSOCKETS` options (all ON by default); switch off those you do not need to save build time. See the [build configuration reference](reference/configuration.md).

## Targets

Each internal micro-benchmark is its own executable named `aeronet-bench-internal-<name>`, defined in `cmake/Benchmarks.cmake` from a source in `benchmarks/internal/`. Which ones are built depends on the enabled features.

| Target | Needs | Description |
| ------ | ----- | ----------- |
| `aeronet-bench-internal-request-parse` | HTTP server | HTTP/1.1 request parsing. |
| `aeronet-bench-internal-router` | HTTP server | Route matching. |
| `aeronet-bench-internal-rate-limit` | HTTP server | Rate limiting stores. |
| `aeronet-bench-internal-date-header-cache` | HTTP server | `Date` header cache. |
| `aeronet-bench-internal-keep-alive-deadline-queue` | HTTP server | Keep-alive deadline queue. |
| `aeronet-bench-internal-zerocopy` | HTTP server | `MSG_ZEROCOPY` sends, including a response streamed to a slow reader. |
| `aeronet-bench-internal-event-loop-poll-timeout` | HTTP server, Linux | Event loop poll timeout computation. |
| `aeronet-bench-internal-http-message-construct`, `-http-message-header-search`, `-http-response-payload`, `-init-try-set-head`, `-reserved-header-check` | none | `HttpResponse` construction, header search and payload handling. |
| `aeronet-bench-internal-http1-response-parse`, `-response-streamer` | HTTP client | Client response parser, response streaming. |
| `aeronet-bench-internal-hpack`, `-http2-frame`, `-http2-flow-control` | HTTP/2 | HPACK, frame codec, flow control. |
| `aeronet-bench-internal-http2-request` | HTTP/2, HTTP server | Server side of a stream, from its HEADERS frame to its response: request head decoding, routing, response encoding. |
| `aeronet-bench-internal-websocket-mask` | WebSocket | WebSocket frame masking. |
| `aeronet-bench-internal-decimal-writer`, `-memory-utils`, `-raw-bytes-growth`, `-search-crlf`, `-string-equal-ignore-case` | none | Low-level helpers: number formatting, copies, buffer growth, CRLF search, case-insensitive comparison. |
| `aeronet-bench-internal-transport-dispatch` | none | Isolated virtual-versus-inline/erased transport benchmark for plain and TLS paths. |
| `aeronet-bench-throughput` | HTTP server | Google Benchmark skeleton for a trivial loopback workload. |
| `aeronet-bench-frameworks` | HTTP server | Comparison with other frameworks, see [Comparative Frameworks](#comparative-frameworks). |
| `run-aeronet-bench` | | Runs request-parse, string-equal-ignore-case, router, event-loop-poll-timeout (Linux) and, with HTTP/2, hpack, http2-frame, http2-flow-control and http2-request, reporting aggregates only. |
| `run-aeronet-bench-json` | | Runs request-parse and writes Google Benchmark JSON to `aeronet-benchmarks.json`. |
| `run-aeronet-bench-throughput` | | Runs the throughput skeleton. |

The scripted server and client benchmarks are built with the same configuration, and driven by their Python runners (see the READMEs linked above).

## Transport Dispatch Benchmark

`aeronet-bench-internal-transport-dispatch` compares the former virtual, heap-owned shape with the final `Transport`
layout: inline plain storage and an erased out-of-line TLS/custom backend. The capability cases issue three queries per
iteration. The write cases use an empty payload so no socket or TLS work obscures dispatch cost. The lifetime cases
include construction and destruction, exposing the allocation removed from the plain path. These cases measure the
transport layer itself, not end-to-end encrypted throughput.

A representative decision run on an AMD Ryzen AI 9 HX PRO 370 performance core with GCC 13.3 and Release + IPO used
seven randomly interleaved, fixed-iteration repetitions and produced these medians:

| Case | Virtual / heap-owned | Inline / erased | Change |
| --- | ---: | ---: | ---: |
| Plain, three capability queries | 3.12 ns | 0.979 ns | -68.6% |
| Plain, empty write | 1.42 ns | 1.25 ns | -12.0% |
| Plain, construct and destroy | 6.62 ns | 1.00 ns | -84.9% |
| TLS, three capability queries | 3.12 ns | 3.35 ns | +7.4% |
| TLS-shaped cached-table empty write | 0.978 ns | 1.00 ns | +2.2% |

The TLS write case uses a synthetic backend with the same erased operation-table dispatch, avoiding handshake and
encryption work. It isolates the added predictable plain/backend branch; it is not a TLS throughput result. The real
`TlsTransport` is used by the TLS capability case. Generated Release assembly contains one discriminator branch and one
cached indirect call on this path.

Before adopting inline storage, long fixed runs of five billion empty writes measured the one-word tagged-owner
prototype. These hardware-counter values per dispatch established the cost of the shared non-virtual dispatch shape:

| Write dispatch | Cycles | Instructions | Branches | Branch misses per billion dispatches |
| --- | ---: | ---: | ---: | ---: |
| Virtual plain | 7.13 | 38.02 | 8.003 | 72,934 |
| Tagged-owner plain | 6.22 | 38.02 | 8.003 | 40,289 |
| Virtual TLS-shaped | 5.06 | 15.01 | 4.002 | 48,842 |
| Tagged-owner TLS-shaped | 5.21 | 20.01 | 5.002 | 49,096 |

That plain path used 12.7% fewer cycles without adding retired instructions or branches. The TLS-shaped path used 2.9%
more cycles because it added five instructions and one correctly predicted branch before its cached indirect call.
Branch-miss rates remained below 0.0013% in every case and were low enough for process and harness noise to affect their
relative ordering. The final inline layout retains the same single-branch TLS dispatch shape; rerun the commands below
when comparing a compiler or storage-layout change.

On the final build, `PlainTransport` is 16 bytes, `Transport` is 24 bytes, and `TlsTransport` remains 48 bytes.
`ConnectionState` grows from 792 to 808 bytes, but a plain connection no longer has a separate backend allocation (16
bytes in the tagged prototype, 24 bytes in the former virtual implementation) or allocator metadata. Compared with the
former virtual implementation, the Release `size` totals for `transport.cpp.o` and `tls-transport.cpp.o` fell from
162,987 to 161,968 bytes and from 168,817 to 167,760 bytes, respectively. The erased backend operation tables are
included in that result.

Build and run stable timing samples in Release mode:

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DAERONET_BUILD_BENCHMARKS=ON
cmake --build build-release --target aeronet-bench-internal-transport-dispatch
./build-release/aeronet-bench-internal-transport-dispatch \
  --benchmark_repetitions=7 --benchmark_min_time=1s --benchmark_report_aggregates_only=true
```

On Linux hosts that permit hardware performance counters, run each case separately with a fixed iteration count. This
keeps the virtual and non-virtual counter totals independent and makes the raw totals directly comparable:

```bash
taskset -c 0 perf stat -e cycles,instructions,branches,branch-misses -- \
  setarch x86_64 -R ./build-release/aeronet-bench-internal-transport-dispatch \
  --benchmark_filter='^BM_VirtualPlainWrite$' --benchmark_min_time=5000000000x
taskset -c 0 perf stat -e cycles,instructions,branches,branch-misses -- \
  setarch x86_64 -R ./build-release/aeronet-bench-internal-transport-dispatch \
  --benchmark_filter='^BM_InlinePlainWrite$' --benchmark_min_time=5000000000x
taskset -c 0 perf stat -e cycles,instructions,branches,branch-misses -- \
  setarch x86_64 -R ./build-release/aeronet-bench-internal-transport-dispatch \
  --benchmark_filter='^BM_VirtualTlsWrite$' --benchmark_min_time=5000000000x
taskset -c 0 perf stat -e cycles,instructions,branches,branch-misses -- \
  setarch x86_64 -R ./build-release/aeronet-bench-internal-transport-dispatch \
  --benchmark_filter='^BM_ErasedTlsWrite$' --benchmark_min_time=5000000000x
```

Use `size` on the same compiler/configuration before and after a dispatch change to track the library object impact:

```bash
size build-release/aeronet/sys/CMakeFiles/aeronet_sys.dir/src/transport.cpp.o \
     build-release/aeronet/tls/CMakeFiles/aeronet_tls.dir/src/tls-transport.cpp.o
```

## JSON Output

Google Benchmark natively supports JSON; any micro-benchmark can write it with `--benchmark_format=json`, and `run-aeronet-bench-json` does it for the request parser:

```bash
cmake --build build --target run-aeronet-bench-json
cat build/aeronet-benchmarks.json
```

## Comparative Frameworks

`aeronet-bench-frameworks` is a Google Benchmark executable that starts an aeronet server and, when the matching `AERONET_BENCH_ENABLE_*` option is ON, drogon, oatpp and cpp-httplib servers in the same process, then performs blocking HTTP/1.1 requests over loopback. Each framework registers the same benchmarks:

- `*BodyMinMax`: the response body size is chosen randomly in a `[min, max]` range (three ranges from a few bytes to 8 MiB).
- `*HeadersMinMax`: a random number of headers with random sizes.
- `*ResponseBuild`: cost of building the response.
- `*BodyMinMaxNoReuse`: the body benchmark with a fresh TCP connection per request, which highlights accept and kernel scheduling overhead compared with keep-alive.

```bash
cmake -S . -B build_bench -DCMAKE_BUILD_TYPE=Release -DAERONET_BUILD_BENCHMARKS=ON
cmake --build build_bench --target aeronet-bench-frameworks
./build_bench/aeronet-bench-frameworks --benchmark_filter='^(Aeronet|Drogon)BodyMinMax/'
```

This in-process comparison is small and informative. The comparison that matters for publishing is the [scripted server benchmark](#ci-and-github-pages), which runs each server as its own process under a load generator and checks [saturation](#saturation-measure-the-server-or-the-client-not-the-noise). Boost.Beast is part of the scripted benchmarks for HTTP/1.1 and WebSocket, and excluded from the HTTP/2 scenarios because it has no HTTP/2 server transport.

## Guidelines for Adding a Benchmark

1. Add the source under `benchmarks/internal/` (micro) or `benchmarks/e2e/` (macro / throughput).
2. Declare its executable in `cmake/Benchmarks.cmake` with `AeronetAddProjectBenchmark(<target> <source> LIBRARIES <libs>)`, linking only the aeronet libraries it exercises (for faster builds), inside the `if` blocks of the features it needs.
3. Prefer small, isolated scopes - avoid mixing multiple subsystems unless explicitly measuring end-to-end.
4. Keep runtime bounded: default iterations should finish in < 2 seconds on a typical dev laptop.
5. Use `benchmark::DoNotOptimize(value)` to prevent undesired optimization.
6. Use counters (`state.counters["name"]`) for derived metrics where helpful.
7. A change that can affect throughput or memory layout should come with a micro-benchmark.

## Caveats

- Loopback measurements elide network variability; real deployment performance can differ.
- Server throughput numbers scale with the number of server threads (`--threads` in the scripted runner); compare servers with the same thread count, and check the [saturation](#saturation-measure-the-server-or-the-client-not-the-noise) flag of each run.
- Comparative numbers should always record compiler, flags, CPU model, and temperature (when publishing externally).

## Profiling with perf

`scripts/profile_benchmark.sh` records a command or an existing PID, exports `perf.script`, and generates
`flamegraph.svg` when Brendan Gregg's FlameGraph scripts are installed. It uses DWARF call graphs by default,
which work with optimized binaries even when frame pointers are unavailable.

Build benchmarks as `RelWithDebInfo` with frame pointers:

```bash
./scripts/profile_benchmark.sh --build
```

Record any finite benchmark command:

```bash
./scripts/profile_benchmark.sh --freq 400 -- ./build-profile/aeronet-bench-internal-router
```

Attach to an already-running server and open the result in Hotspot:

```bash
./scripts/profile_benchmark.sh --pid "$(pidof aeronet-bench-server)" --hotspot
```

Hotspot is discovered on `PATH`, under `~/Downloads/hotspot-*.AppImage`, or under
`~/Applications/hotspot-*.AppImage`. Use `--hotspot-bin FILE` to override it.

### Scripted server and client profiles

The scripted runners can profile the measured process directly. Server runs attach after warmup and record
the server while `wrk` or `h2load` drives it. Client runs record the selected finite client driver, including
its in-process warmup, and do not record the shared server.

```bash
# One server workload, with an SVG flamegraph and Hotspot view.
python3 benchmarks/scripted-servers/run_benchmarks.py \
  --server aeronet --scenario headers --duration 15s \
  --profile --profile-install-flamegraph --profile-hotspot

# One client workload.
python3 benchmarks/scripted-clients/run_client_benchmarks.py \
  --client aeronet --scenario small-get --duration 15s \
  --profile --profile-install-flamegraph --profile-hotspot
```

Profiling artifacts are stored below the runner's output directory:

```text
results/profiles/<run>/<protocol>/<server-or-client>/<scenario>/
  perf.data
  perf.script
  flamegraph.svg
```

With `--repeat N`, server profiles get an additional `sample-N/` directory so each measured run remains
separate. Avoid `--profile-hotspot` with broad multi-scenario runs because it opens one GUI window per profile.

### Permissions and tools

Install perf using the package matching the running kernel. On Ubuntu or Debian:

```bash
sudo apt install linux-tools-$(uname -r) linux-tools-common
```

The scripted runners cannot prompt for `sudo` after starting child processes. Allow per-process perf recording
before running them:

```bash
sudo sysctl kernel.perf_event_paranoid=1
```

For a single direct command, `profile_benchmark.sh --sudo -- ...` is also supported. The runner preflight and
the helper both report the current `kernel.perf_event_paranoid` value when access is denied.

Pass `--profile-install-flamegraph` in a scripted run, or `--install-flamegraph` to the helper, to clone
FlameGraph into `${XDG_CACHE_HOME:-~/.cache}/aeronet/FlameGraph`. Existing recordings can be processed again
without rerunning the workload:

```bash
./scripts/profile_benchmark.sh --input path/to/perf.data --install-flamegraph --hotspot
```

The helper also accepts `--flamegraph-dir DIR`, `--call-graph dwarf|fp|lbr`, `--event EVENT`, and
`--output-dir DIR`; run it with `--help` for the complete interface.

## Other diagnostics

When a regression is detected, besides the flamegraphs above:

- Memory profile with Valgrind Massif: `valgrind --tool=massif --massif-out-file=massif.out <aeronet_server>`, then `ms_print massif.out`.
- Syscall overhead with strace: `strace -e trace=sendfile,writev,write -c <aeronet_server>`. For files served over plaintext, `sendfile` should appear instead of `write`/`writev` of the file content.
