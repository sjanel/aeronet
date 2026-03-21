# aeronet Benchmarks

This document describes the purpose, scope, and usage of the optional benchmarking
suite for aeronet.

## Goals

- Track performance regressions for core HTTP request handling and (later) streaming/TLS.
- Provide comparative harnesses against other C++ HTTP frameworks (planned: oatpp, drogon).
- Keep CI lightweight: benchmarks are **not** built in CI by default.
- Offer reproducible local runs with JSON output for ad‑hoc analysis.

## Non‑Goals (Current Phase)

- Producing authoritative cross‑platform numbers (cloud CI noise is high).
- Shipping benchmark binaries in packages.
- Providing full-feature client load generation (wrk/vegeta do that better externally).

## Build Activation

By default benchmarks build only when aeronet is the main project (top‑level) OR when you
explicitly enable them:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAERONET_BUILD_BENCHMARKS=ON
cmake --build build --target run-aeronet-bench
```

In CI (environment variable `CI` defined) they are skipped unless you override:

```bash
-DAERONET_BENCH_FORCE_CI=ON
```

## Targets

| Target | Description |
| ------ | ----------- |
| `aeronet-bench-internal` | Google Benchmark based micro / roundtrip benchmarks (contains `BENCHMARK_MAIN`). |
| `aeronet-bench-throughput` | Simple standalone executable printing JSON-ish timing for a trivial loopback workload. |
| `run-aeronet-bench` | Convenience target: runs internal benchmarks (aggregates only). |
| `run-aeronet-bench-json` | Emits Google Benchmark JSON (`aeronet-benchmarks.json`). |
| `run-aeronet-bench-throughput` | Runs the throughput skeleton. |
| `aeronet-bench-frameworks` | Comparative simple GET size= handler (aeronet + optional drogon/oatpp). |
| `run-aeronet-bench-frameworks` | Runs comparative benchmark with default args. |

## JSON Output

Generate structured benchmark data:

```bash
cmake --build build --target run-aeronet-bench-json
cat build/aeronet-benchmarks.json
```

Google Benchmark natively supports JSON; we simply redirect stdout.

## Comparative Frameworks

An initial comparative benchmark `aeronet-bench-frameworks` is available. It spins up:

- aeronet (always) – `/data?size=N` returning an iota-generated `std::string` of length N.
- Drogon (if `-DAERONET_BENCH_ENABLE_DROGON=ON`) – identical endpoint.
- Oatpp (if `-DAERONET_BENCH_ENABLE_OATPP=ON`) – identical endpoint.

Each iteration selects a random payload size in `[min,max]` and performs a blocking HTTP/1.1 request over loopback. Metrics collected:

- Total wall time
- Requests per second
- Aggregate bytes and MB/s (body bytes only)

Activate extra frameworks:

```bash
cmake -S . -B build_bench -DCMAKE_BUILD_TYPE=Release -DAERONET_BUILD_BENCHMARKS=ON \
  -DAERONET_BENCH_ENABLE_DROGON=ON -DAERONET_BENCH_ENABLE_OATPP=ON
cmake --build build_bench --target aeronet-bench-frameworks
./build_bench/aeronet-bench-frameworks --iters=5000 --min=64 --max=8192
```

Planned enhancements:

- Percentile latency (collect micro timings per request)
- Concurrent client connections
- Streaming & TLS variants (requires `AERONET_ENABLE_OPENSSL=ON`)

## Planned Roadmap

| Phase | Item | Status |
| ----- | ---- | ------ |
| A | Initial harness + minimal roundtrip bench | DONE |
| A | Throughput skeleton | DONE |
| B | Parse-only microbench (expose internal parser) | TODO |
| B | Multi-threaded in-process client loop | TODO |
| C | Simulated degraded network (latency, bandwidth) | TODO |
| C | Streaming benchmarks | TODO |
| D | Comparative: oatpp / drogon basic handler | PARTIAL (basic size endpoint & driver) |
| D | JSON consolidation & history (append-only .jsonl) | TODO |
| E | TLS handshake & request benchmarks | TODO |

## Guidelines for Adding a Benchmark

1. Add source under `benchmarks/internal/` (micro) or `benchmarks/e2e/` (macro / throughput).
2. Append the file to `AERONET_BENCH_INTERNAL_SOURCES` (for microbench) or create a new executable.
3. Prefer small, isolated scopes—avoid mixing multiple subsystems unless explicitly measuring end-to-end.
4. Keep runtime bounded: default iterations should finish in < 2 seconds on a typical dev laptop.
5. Use `benchmark::DoNotOptimize(value)` to prevent undesired optimization.
6. Use counters (`state.counters["name"]`) for derived metrics where helpful.

## Caveats

- Loopback measurements elide network variability; real deployment performance can differ.
- Single-thread server design: multi-core scaling requires multiple processes/instances; benchmarks will eventually include multi-instance harnesses.
- Comparative numbers should always record compiler, flags, CPU model, and temperature (when publishing externally).

## Example Quick Run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAERONET_BUILD_BENCHMARKS=ON
cmake --build build --target run-aeronet-bench
./build/aeronet-bench-throughput
```

## Future Ideas

- Optional integration with `perf` / hardware counters (off by default).
- Flamegraph helper script capturing `perf record -g` around a chosen benchmark.
- Automatic detection of regression (simple % threshold) when explicitly requested.

## Benchmark profiling with perf and kcachegrind

This file explains how to profile the `aeronet` benchmarks using `perf` and visualize the results with `kcachegrind` (via callgrind format) or with FlameGraph.

### Quick helper

We include a small helper script at `scripts/profile_benchmark.sh` that automates a typical workflow:

- Build (optional) with frame pointers and debuginfo
- Run `perf record` with call-graph sampling
- Convert the `perf.data` to a callgrind file (if conversion tools are available) or to a FlameGraph SVG

### Usage examples

1) Build and profile the throughput benchmark (recommended):

```bash
# from repository root
./scripts/profile_benchmark.sh --build -- ./build/benchmarks/aeronet-bench-throughput --duration 10
```

1) Profile an already-built binary (sample frequency 400Hz):

```bash
./scripts/profile_benchmark.sh --freq 400 -- ./build/benchmarks/aeronet-bench-throughput --duration 10
```

### What the script does

- Runs `perf record -F <freq> -g -- <binary> ...` as root (sudo) to capture user-space stacks
- Writes `perf.data.YYYYMMDD-HHMMSS` and `perf_script.YYYYMMDD-HHMMSS.txt`
- Attempts to convert the `perf script` output to callgrind format using one of:
  - `perf2calltree` (packaged with kcachegrind on some distros)
  - `/usr/share/kcachegrind/perf2calltree` (if present)
  - `gprof2calltree` (Python tool)
- If conversion tools are not found, it can generate a FlameGraph SVG if you clone Brendan Gregg's FlameGraph under `scripts/FlameGraph`.

### Installing useful tools on Ubuntu

```bash
sudo apt update
# perf (kernel tools), kcachegrind, flamegraph helper
sudo apt install -y linux-tools-$(uname -r) linux-tools-common kcachegrind python3-pip
pip3 install gprof2calltree
```

If `perf2calltree` is missing in your distro package, you can install `kcachegrind` which may include it, or fetch `perf2calltree` from the perf-tools or kcachegrind source.

### Converting manually

If you prefer to do things by hand:

1. Record:

```bash
sudo perf record -F 200 -g -o perf.data -- ./build/benchmarks/aeronet-bench-throughput --duration 10
```

1. Export perf script:

```bash
perf script -i perf.data > perf_script.txt
```

1. Convert to callgrind (one of):

```bash
# If perf2calltree present
perf script -i perf.data | perf2calltree > callgrind.out

# Or using gprof2calltree
perf script -i perf.data > perf_for_gprof.txt
gprof2calltree -i perf_for_gprof.txt -o callgrind.out
```

1. Open callgrind in kcachegrind:

```bash
kcachegrind callgrind.out
```

### FlameGraph (alternative visualization)

Clone FlameGraph and run stackcollapse & flamegraph:

```bash
git clone https://github.com/brendangregg/FlameGraph.git scripts/FlameGraph
perf script -i perf.data | scripts/FlameGraph/stackcollapse-perf.pl | scripts/FlameGraph/flamegraph.pl > flamegraph.svg
# open flamegraph.svg in a browser
```

### Notes and tips

- Build with `-fno-omit-frame-pointer` for the best call stacks if your compiler and target support it (x86_64). The helper script configures that when run with `--build`.
- Use `RelWithDebInfo` to keep optimizations but also have debug symbols.
- For in-depth CPU tracing of syscalls or kernel stacks, you can drop `-g` and collect kernel stacks too, but that makes conversion harder.
- When running in containers or VMs, ensure `perf_event` access is allowed (sometimes `sysctl kernel.perf_event_paranoid=1` or lower is required).

If you'd like, I can:

- Add a small CMake target to build all benchmark binaries in `build/benchmarks` in one step
- Run a sample profiling pass here and attach the generated `callgrind.out` and `flamegraph.svg` (if you consent to uploading artifacts)

## HTTP/2 Performance Testing Plan with h2load

**Purpose:** Establish performance baseline and regression detection for HTTP/2

### Overview

This document outlines HTTP/2 performance testing scenarios using h2load, a benchmark tool specifically designed for HTTP/2 load testing.

### h2load Test Scenarios

#### Scenario 1: Baseline RPC-Style (Default)

**Test:** Simple JSON request/response with stream multiplexing

```bash
h2load -n 100000 -c 100 -m 100 \
  -H 'Content-Type: application/json' \
  https://localhost:8443/api/users/1
```

- **-n 100000** : Total requests
- **-c 100** : Concurrent connections
- **-m 100** : Max streams per connection (multiplexing)

**Metrics to Track:**

- Requests per second (req/s)
- Average latency (ms)
- Min/Max latency
- Memory usage per connection
- CPU utilization

**Baseline Goal:** Establish baseline metrics for future regression detection

---

#### Scenario 2: Large Body Streaming

**Test:** Large payload response (e.g., 10 MiB download)

```bash
h2load -n 1000 -c 50 -m 50 \
  -H 'Accept-Encoding: gzip,zstd' \
  https://localhost:8443/files/large.bin
```

**Metrics to Track:**

- Throughput (bytes/sec)
- Latency under streaming
- Compression efficiency
- Memory allocation patterns during streaming

**Expected:** Zero-copy sendfile should be observable via strace/perf

---

#### Scenario 3: Compression Efficiency

**Test:** Compression negotiation and throughput

```bash
h2load -n 10000 -c 100 -m 100 \
  -H 'Accept-Encoding: zstd' \
  https://localhost:8443/api/small-payload
```

**Metrics to Track:**

- Compression ratio (bytes sent / original)
- CPU overhead of compression
- Throughput (req/s) with/without compression
- Wall-clock time

**Expected:** zstd to have best compression/speed tradeoff

---

#### Scenario 4: High Stream Multiplexing

**Test:** Maximum concurrent streams stress

```bash
h2load -n 50000 -c 10 -m 500 \
  https://localhost:8443/api/echo
```

- **-m 500** : Max concurrent streams (high multiplexing)
- **-c 10** : Fewer connections (more multiplexing per conn)

**Metrics to Track:**

- Requests per second
- Stream state management overhead
- Flow control window utilization
- HPACK decoder efficiency

**Expected:** Near-linear performance scaling with streams (no bottlenecks)

---

#### Scenario 5: Small Payload Latency

**Test:** Ultra-low-latency requests (e.g., health checks, metrics)

```bash
h2load -n 100000 -c 100 -m 100 \
  https://localhost:8443/health
```

**Metrics to Track:**

- Tail latencies (p50, p95, p99)
- Min latency (event loop responsiveness)
- GC/allocation pauses

**Expected:** Sub-millisecond latencies (< 1ms p99)

---

#### Scenario 6: TLS Handshake Overhead

**Test:** Connection establishment with TLS

```bash
# Warm handshakes (reused connections)
h2load -n 10000 -c 100 -m 100 \
  https://localhost:8443/api/echo

# Cold handshakes (new connections every N requests)
h2load -n 10000 -c 100 -m 1 \
  https://localhost:8443/api/echo
```

**Metrics to Track:**

- Handshake time (ms)
- Time to first byte (TTFB) with/without handshake
- TLS 1.3 0-RTT impact (if enabled)
- Session ticket resumption benefit

**Expected:** <50ms handshake, <10ms TTFB for subsequent requests

---

#### Scenario 7: Header Compression (HPACK)

**Test:** Header compression efficiency

```bash
# Small headers (default)
h2load -n 10000 -c 100 -m 100 \
  https://localhost:8443/api/echo

# Large headers (many custom headers)
h2load -n 10000 -c 100 -m 100 \
  -H 'X-Custom-1: value-1' \
  -H 'X-Custom-2: value-2' \
  ... (repeat 50+ custom headers) \
  https://localhost:8443/api/echo
```

**Metrics to Track:**

- Bytes per request (with large headers)
- Compression ratio (static + dynamic table)
- Dynamic table eviction rate
- HPACK encoder/decoder CPU time

**Expected:** >60% header compression ratio

---

#### Scenario 8: Flow Control Behavior

**Test:** Slow client reading (flow control window exhaustion)

```bash
# Simulate slow reading: --rate-period=1ms (one read per millisecond)
h2load -n 1000 -c 10 -m 10 \
  --rate=1000 \
  https://localhost:8443/files/medium-file
```

**Metrics to Track:**

- Throughput under flow control constraints
- WINDOW_UPDATE frame frequency
- Buffer memory usage
- Stream stall time

**Expected:** Proper backpressure (no excessive buffering)

### Performance Regression Detection (ideas)

#### Baseline Metrics

To be established in first run:

```json
{
  "scenario": "baseline_rpc",
  "date": "2026-01-17",
  "metrics": {
    "req_per_sec": 45000,  // Expected value
    "avg_latency_ms": 2.3,
    "p99_latency_ms": 5.1,
    "memory_per_conn_kb": 128
  }
}
```

#### Regression Threshold

Fail build if:

- req/s < baseline * 0.95 (5% regression)
- p99_latency_ms > baseline * 1.1 (10% increase)
- memory_per_conn > baseline * 1.2 (20% increase)

---

### Profiling & Diagnostics

When performance regressions are detected:

#### 1. CPU Profiling with perf

```bash
perf record -F 99 -p <server_pid> -- h2load ... 
perf report
```

#### 2. Flame Graph Generation

```bash
perf script | inferno-collapse-perf | inferno-flamegraph > /tmp/perf.svg
```

#### 3. Memory Profiling with Valgrind/Massif

```bash
valgrind --tool=massif --massif-out-file=massif.out <aeronet_server>
ms_print massif.out
```

#### 4. strace for Syscall Overhead

```bash
strace -e trace=sendfile,writev,write -c <aeronet_server>
```
