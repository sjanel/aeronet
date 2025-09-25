# Aeronet Benchmarks

This document describes the purpose, scope, and usage of the optional benchmarking
suite for Aeronet.

## Goals

* Track performance regressions for core HTTP request handling and (later) streaming/TLS.
* Provide comparative harnesses against other C++ HTTP frameworks (planned: oatpp, drogon).
* Keep CI lightweight: benchmarks are **not** built in CI by default.
* Offer reproducible local runs with JSON output for ad‑hoc analysis.

## Non‑Goals (Current Phase)

* Producing authoritative cross‑platform numbers (cloud CI noise is high).
* Shipping benchmark binaries in packages.
* Providing full-feature client load generation (wrk/vegeta do that better externally).

## Build Activation

By default benchmarks build only when Aeronet is the main project (top‑level) OR when you
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
| `aeronet-bench-frameworks` | Comparative simple GET size= handler (Aeronet + optional drogon/oatpp). |
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

* Aeronet (always) – `/data?size=N` returning an iota-generated `std::string` of length N.
* Drogon (if `-DAERONET_BENCH_ENABLE_DROGON=ON`) – identical endpoint.
* Oatpp (if `-DAERONET_BENCH_ENABLE_OATPP=ON`) – identical endpoint.

Each iteration selects a random payload size in `[min,max]` and performs a blocking HTTP/1.1 request over loopback. Metrics collected:

* Total wall time
* Requests per second
* Aggregate bytes and MB/s (body bytes only)

Activate extra frameworks:

```bash
cmake -S . -B build_bench -DCMAKE_BUILD_TYPE=Release -DAERONET_BUILD_BENCHMARKS=ON \
  -DAERONET_BENCH_ENABLE_DROGON=ON -DAERONET_BENCH_ENABLE_OATPP=ON
cmake --build build_bench --target aeronet-bench-frameworks
./build_bench/aeronet-bench-frameworks --iters=5000 --min=64 --max=8192
```

Planned enhancements:

* Percentile latency (collect micro timings per request)
* Concurrent client connections
* Streaming & TLS variants (requires `AERONET_ENABLE_OPENSSL=ON`)

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

* Loopback measurements elide network variability; real deployment performance can differ.
* Single-thread server design: multi-core scaling requires multiple processes/instances; benchmarks will eventually include multi-instance harnesses.
* Comparative numbers should always record compiler, flags, CPU model, and temperature (when publishing externally).

## Example Quick Run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAERONET_BUILD_BENCHMARKS=ON
cmake --build build --target run-aeronet-bench
./build/aeronet-bench-throughput
```

## Future Ideas

* Optional integration with `perf` / hardware counters (off by default).
* Flamegraph helper script capturing `perf record -g` around a chosen benchmark.
* Automatic detection of regression (simple % threshold) when explicitly requested.

---

Feel free to extend this doc as new benchmarks land.
