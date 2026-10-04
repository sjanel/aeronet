# Scripted Server Benchmarks

This directory contains standalone HTTP server executables and `wrk` benchmark scripts to compare **aeronet** against other high-performance HTTP frameworks, possibily implemented in different programming languages.

## Overview

Unlike Google Benchmark (which measures internal latencies), this setup uses **external load generators** to measure real-world throughput and latency under stress. The primary tools are:

- [`wrk`](https://github.com/wg/wrk) - HTTP/1.1 benchmarking with LuaJIT scripting
- [`h2load`](https://nghttp2.org/documentation/h2load-howto.html) - HTTP/2 benchmarking (part of nghttp2), supports both cleartext h2c and TLS h2

## What is wrk and LuaJIT?

**wrk** is a multi-threaded HTTP benchmarking tool capable of generating significant load. It uses an event-driven model with epoll/kqueue and can saturate modern multi-core systems.

**LuaJIT** is a Just-In-Time compiler for Lua that wrk embeds. This lets you write custom scripts to:

- Generate dynamic requests (varying headers, bodies, paths)
- Implement complex scenarios (authentication flows, session handling)
- Process and aggregate response data
- Create realistic mixed workloads

### wrk Lua API

wrk exposes several callbacks you can override:

```lua
-- Called once to build the initial request
function request()
  return wrk.format(method, path, headers, body)
end

-- Called for each response received
function response(status, headers, body)
  -- Process response
end

-- Called once before starting (per thread)
function init(args)
  -- Initialize thread-local state
end

-- Called once after completion
function done(summary, latency, requests)
  -- Print custom statistics
end

-- Called periodically to setup next request
function request()
  -- Return next request to send
end
```

## Prerequisites

### Install wrk

```bash
# Ubuntu/Debian
sudo apt-get install wrk

# macOS
brew install wrk

# From source (recommended for latest features)
git clone https://github.com/wg/wrk.git
cd wrk
make -j$(nproc)
sudo cp wrk /usr/local/bin/
```

### Install h2load (for HTTP/2 benchmarks)

```bash
# Ubuntu/Debian
sudo apt-get install nghttp2-client

# macOS
brew install nghttp2

# Verify
h2load --version
```

### Python runtime

The orchestrator (`run_benchmarks.py`) targets modern CPython (3.12+).
Any recent Python 3 interpreter with the standard library is sufficient;
no additional packages are required.

### Build the benchmark servers

```bash
cd /path/to/aeronet
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DAERONET_BUILD_BENCHMARKS=ON
ninja aeronet-bench-server drogon-bench-server
```

### Pistache (built from source)

Pistache is automatically fetched and built from source via CMake FetchContent when `AERONET_BENCH_ENABLE_PISTACHE=ON`. This ensures static linking and LTO can be enabled for a fair comparison. SSL is disabled for HTTP-only benchmarking.

```bash
# In your build directory
cmake .. -DAERONET_BENCH_ENABLE_PISTACHE=ON
ninja pistache-bench-server
```

**Note:** Pistache requires explicit `Connection: keep-alive` header in requests, otherwise it defaults to `Connection: Close` which severely impacts performance. All Lua benchmark scripts include this header automatically. The `mixed_workload.lua` scenario also injects a small percentage of `Connection: close` requests to simulate real-world connection churn.

### Crow (built from source)

Crow (CrowCpp/Crow - the maintained fork) is automatically fetched and built from source via CMake FetchContent when `AERONET_BENCH_ENABLE_CROW=ON`. Crow is a header-only C++ web framework similar to Python Flask.

```bash
# In your build directory
cmake .. -DAERONET_BENCH_ENABLE_CROW=ON
ninja crow-bench-server
```

### Boost.Beast

Boost.Beast support is enabled with `AERONET_BENCH_ENABLE_BEAST=ON` and builds
the `beast-bench-server` scripted benchmark target.

```bash
# In your build directory
cmake .. -DAERONET_BENCH_ENABLE_BEAST=ON
ninja beast-bench-server
```

## Benchmark Scenarios

### 1. Pure Header Parsing (`headers_stress.lua`)

Tests header parsing performance with many large headers.

```bash
# Start server
./aeronet-bench-server &

# Run benchmark
wrk -t4 -c100 -d30s -s lua/headers_stress.lua http://127.0.0.1:8080/headers
```

### 2. Large Body POST (`large_body.lua`)

Tests body handling with single large payloads.

```bash
wrk -t4 -c100 -d30s -s lua/large_body.lua http://127.0.0.1:8080/uppercase
```

### 3. High Request Rate - Small Static (`static_routes.lua`)

Tests routing and response writing speed with minimal payloads.

```bash
wrk -t4 -c100 -d30s -s lua/static_routes.lua http://127.0.0.1:8080/ping
```

### 4. CPU-Bound Handler (`cpu_bound.lua`)

Tests scheduling overhead with computationally expensive handlers.

```bash
wrk -t4 -c100 -d30s -s lua/cpu_bound.lua http://127.0.0.1:8080/compute
```

### 5. Mixed Workload (`mixed_workload.lua`)

Simulates realistic microservice traffic patterns.

```bash
wrk -t4 -c100 -d30s -s lua/mixed_workload.lua http://127.0.0.1:8080/

# Optional: add connection churn (percentage of requests using Connection: close)
wrk -t4 -c100 -d30s -s lua/mixed_workload.lua http://127.0.0.1:8080/ --close-ratio 20
```

### 6. Static File Serving (`static_files.lua`)

Tests static file handler with various file sizes. Requires setup first.

```bash
# Generate test files
./setup_bench_resources.py

# Start server with static file directory
./aeronet-bench-server --static ./static &

# Run benchmark
wrk -t4 -c100 -d30s -s lua/static_files.lua http://127.0.0.1:8080/index.html
```

> **Note - files are kept small (1 MiB).** Fast servers (aeronet, drogon, ...) serve
> static files with `sendfile()`, so the server does almost no per-request work while
> `wrk` still has to copy every response byte out of the kernel. With multi-megabyte
> files the load generator cannot saturate the server whatever its thread count, so the
> scenario serves `medium.bin` (1 MiB). Like every scenario, it reports the server CPU
> utilization so that a client-bound measurement is visible (see
> [Saturation checks](#saturation-checks)).

### 7. Router Stress Test (`routing_stress.lua`)

Tests router lookup performance with large route tables (1000+ routes).

```bash
# Start server with many routes
./aeronet-bench-server --routes 1000 &

# Run benchmark
wrk -t4 -c100 -d30s -s lua/routing_stress.lua http://127.0.0.1:8080/r0
```

## Running the Full Benchmark Suite

### HTTP/1.1 (wrk)

Use the Python runner to benchmark all servers and scenarios:

```bash
run_benchmarks.py
```

### HTTP/2 (h2load)

The same runner supports HTTP/2 benchmarking via h2load. Two protocol modes are available:

- **h2c** - HTTP/2 over cleartext TCP (no TLS overhead)
- **h2-tls** - HTTP/2 over TLS (production-realistic)

```bash
# h2c mode (cleartext HTTP/2)
run_benchmarks.py --protocol h2c

# h2-tls mode (HTTP/2 over TLS)
run_benchmarks.py --protocol h2-tls

# Control multiplexed streams per connection
run_benchmarks.py --protocol h2c --h2-streams 10

# Combine with server/scenario filters
run_benchmarks.py --protocol h2c --server aeronet,go --scenario headers,mixed
```

**HTTP/2 framework support:**

| Server | h2c | h2-tls | Notes |
| -------- | ----- | -------- | ------- |
| aeronet | Yes | Yes | Full HTTP/2 support |
| rust | Yes | Yes | hyper-util auto-detect |
| undertow | Yes | Yes | UndertowOptions.ENABLE_HTTP2 |
| go | Yes | Yes | golang.org/x/net/http2/h2c |
| python | Yes | Yes | Hypercorn replaces uvicorn |

Pistache, Drogon and Crow are excluded from HTTP/2 benchmarks (no H2 support).
Boost.Beast is also excluded from HTTP/2 benchmarks (Beast has no HTTP/2 server transport).

### Options

```bash
run_benchmarks.py --threads 4        # Server worker threads (and CPUs reserved for the server)
run_benchmarks.py --loadgen-threads 6 # wrk/h2load threads (default: automatic, up to 3 per server thread)
run_benchmarks.py --connections 100  # Number of connections (default: 100)
run_benchmarks.py --duration 30s     # Benchmark duration per scenario
run_benchmarks.py --warmup 5s        # Warmup duration before each run
run_benchmarks.py --server aeronet   # Only benchmark specific server(s)
run_benchmarks.py --scenario headers # Only run specific scenario(s)
run_benchmarks.py --output results/  # Output directory for result artifacts
run_benchmarks.py --protocol http1   # Protocol: http1 (default), h2c, h2-tls
run_benchmarks.py --h2-streams 10    # Multiplexed streams per h2 connection (default: 10)
run_benchmarks.py --repeat 3         # Take N samples per case, report the median (default: 1)
run_benchmarks.py --no-cpu-pin       # Disable automatic taskset pinning of server vs load generator
run_benchmarks.py --profile          # Record each measured server/scenario with perf
run_benchmarks.py --profile-install-flamegraph # Install FlameGraph scripts and generate SVGs
run_benchmarks.py --profile-hotspot  # Open each perf.data in Hotspot

# Run multiple values (comma-separated)
run_benchmarks.py --server aeronet,beast,python --scenario headers,body,routing

# Available scenarios: headers, body, static, cpu, mixed, files, routing, tls
```

### Saturation checks

A server benchmark measures the server only if the server is the bottleneck. On Linux (with `taskset`),
`run_benchmarks.py` and `run_ws_benchmarks.py` therefore reserve CPUs for each side
(`bench_utils.plan_server_benchmark_cpus`): the server gets `--threads` CPUs, one per physical core when
possible, and the load generator every remaining CPU it needs (up to 3 per server thread), CPUs of other
physical cores first and SMT siblings of the server last. The server CPU utilization (process tree, from
`/proc`) is measured over each run: below 90% of its CPUs, the run is flagged as not saturated in the console,
in the JSON summary (`results.<scenario>.server_cpu`) and in the HTML report (CPU table and warning banner).
On a 4-vCPU CI runner (2 physical cores), 1 server thread and 3 load generator threads keep the server at
94-100% in every HTTP/1.1, h2c and WebSocket scenario.

For a focused CPU profile, enable perf access first and select one server/scenario:

```bash
sudo sysctl kernel.perf_event_paranoid=1
run_benchmarks.py --server aeronet --scenario headers --duration 15s \
  --profile --profile-install-flamegraph --profile-hotspot
```

Warmup traffic is excluded. Profiles are written below `OUTPUT/profiles/<run>/<protocol>/<server>/<scenario>/`.
With `--repeat N`, each measured sample gets its own `sample-N/` directory. See
[`../../docs/BENCHMARKS.md`](../../docs/BENCHMARKS.md#profiling-with-perf) for call-graph settings, direct PID
attachment, existing-data processing, and Hotspot discovery.

## Server Implementations

All servers implement identical endpoints:

| Endpoint | Method | Description |
| ---------- | -------- | ------------- |
| `/ping` | GET | Returns "pong" (minimal latency test) |
| `/headers` | GET | Returns N headers based on `?count=N` query param |
| `/uppercase` | POST | Converts request body to uppercase |
| `/compute` | GET | CPU-intensive computation (Fibonacci, hashing) |
| `/json` | GET | Returns JSON response |
| `/delay` | GET | Artificial delay via `?ms=N` query param |
| `/body` | GET | Returns body of `?size=N` bytes |
| `/status` | GET | Health check with JSON response |
| `/*` | GET | Static file serving (aeronet only, with `--static DIR`) |
| `/r{N}` | GET | Routing test routes (aeronet only, with `--routes N`) |
| `/users/{id}/posts/{post}` | GET | Pattern-matched route (aeronet only) |
| `/ws-uncompressed` | WS | WebSocket echo endpoint without permessage-deflate |
| `/ws-compressed` | WS | WebSocket echo endpoint with permessage-deflate (framework-dependent) |

### Supported Servers

| Server | Language | File | Notes |
| -------- | ---------- | ------ | ------- |
| aeronet | C++ | `aeronet_server.cpp` | Primary benchmark target |
| drogon | C++ | `drogon_server.cpp` | Popular C++ async framework |
| uwebsockets | C++ | `uwebsockets_server.cpp` | High-perf WebSocket-first framework |
| pistache | C++ | `pistache_server.cpp` | REST framework for C++ |
| crow | C++ | `crow_server.cpp` | Header-only C++ microframework |
| beast | C++ | `beast_server.cpp` | Boost.Beast HTTP/1.1 + WebSocket |
| rust | Rust | `rust_server/` | axum async framework |
| undertow | Java | `undertow_server/UndertowBenchServer.java` | High-perf Java NIO server |
| go | Go | `go_server.go` | Standard library net/http |
| python | Python | `python_server.py` | uvicorn + starlette (async) |

Each server gets `--threads` request-processing threads, and is configured so that only its own work limits it
(see [Saturation checks](#saturation-checks)):

- **crow** keeps one of its `concurrency` threads for accepting connections: it runs with `--threads + 1`. Crow never
  enables `TCP_NODELAY`, so a response sent in several writes (a few dozen headers) waited for the client's delayed
  ACK (40 ms) and the server idled: the bench server sets it on the listening socket, inherited by accepted
  connections on Linux.
- **go** sets `GOMAXPROCS` to `--threads` (sysmon and blocked-syscall threads are mostly idle, GC workers take their
  share of the `GOMAXPROCS` slots).
- **pistache** sends the generated `/headers` as typed headers: raw headers (`Header::Raw`) are silently dropped from
  its responses.
- pistache answers `Connection: close` requests without closing the connection: wrk closes it and reconnects, holding
  the `TIME_WAIT` state, so its `mixed` scenario stays below saturation.

### Building/Running Non-C++ Servers

**Go server:**

```bash
# Build
cd benchmarks/scripted-servers
go build -o go-bench-server go_server.go

# Run
./go-bench-server --port 8083 --threads 4
```

**Java Undertow server:**

```bash
# Download dependencies (one time, see run_benchmarks.py for the exact jar list)

# Compile
# Use a classpath that includes the current directory and all JARs. Some
# shells expand the wildcard differently; the following works on Linux/Bash:
javac -cp ".:*" UndertowBenchServer.java

# Run
# Include current directory and jars on the classpath. Use the same wildcard
# expansion when starting the server:
java -cp ".:*" UndertowBenchServer --port 8082 --threads 4
```

**Python server:**

```bash
# Install dependencies
pip install uvicorn starlette

# Run
cd benchmarks/scripted-servers
python3 python_server.py --port 8084 --threads 4
```

**Rust server:**

```bash
# Requires rustup/cargo
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# Build (from rust_server directory)
cd benchmarks/scripted-servers/rust_server
cargo build --release

# Run
./target/release/rust-bench-server --port 8086
```

## Interpreting Results

wrk outputs:

```bash
Running 30s test @ http://127.0.0.1:8080/ping
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   245.23us  364.12us  12.34ms   91.23%
    Req/Sec    65.12k     5.23k   78.45k    68.75%
  7823456 requests in 30.00s, 1.23GB read
Requests/sec: 260781.87
Transfer/sec:     42.01MB
```

Key metrics:

- **Requests/sec**: Primary throughput metric
- **Latency Avg/Max**: Response time distribution
- **Transfer/sec**: Network throughput

## Memory Metrics

After the wrk tables complete the Python runner prints and records an additional memory usage summary
for each scenario/server combination. The table is derived directly from `/proc/<pid>/status` and includes:

- **RSS**: resident set size in MB (current resident memory)
- **Peak**: VmPeak, the largest address space seen during the run
- **VMHWM**: high-water mark of RSS during the process lifetime (VmHWM)
- **VMSize**: total virtual address space (VmSize)
- **Swap**: amount of swapped memory (VmSwap)

## Tips for Accurate Benchmarking

1. **Disable CPU frequency scaling**: `sudo cpupower frequency-set -g performance`
2. **Pin processes to cores**: Use `taskset` to avoid NUMA effects
3. **Warm up**: Run a short warmup before the real test (especially for JIT runtimes)
4. **Multiple runs**: Take the median of 3-5 runs — `--repeat N` does this automatically (runs each case N times and reports the median-throughput sample). The CI weekly/manual runs use it for steadier numbers; the run date is recorded in every result file and shown on the rendered HTML report.
5. **Same machine vs remote**: Local tests eliminate network variance but may cause resource contention
6. **Check for errors**: Verify `Non-2xx responses` count is zero
7. **Use keep-alive**: All Lua scripts include `Connection: keep-alive` header. Some servers (e.g., Pistache) default to `Connection: Close` if no header is sent, which drastically reduces throughput

### CPU And Process Pinning

For repeatable, low-variance measurements on modern hybrid CPUs (for example 12th Gen Intel i7 with P/E cores), follow these steps to inspect, set, and pin CPU behavior before running benchmarks.

- **Inspect CPU topology and max frequencies**: identify P-cores (high-frequency) vs E-cores.

```bash
lscpu -e
for c in /sys/devices/system/cpu/cpu[0-9]*; do
  printf "%s %s\n" "${c##*/}" "$(cat $c/cpufreq/cpuinfo_max_freq 2>/dev/null || echo NA)"
done | sort -k2 -nr
```

- **Set performance governor**: lock the CPU to performance governor to avoid frequency scaling noise.

```bash
sudo cpupower frequency-set -g performance
```

If `cpupower` is not available, use sysfs:

```bash
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

- **Disable turbo (optional, for determinism)**:

```bash
# try intel_pstate no_turbo
sudo sh -c 'echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo' \
  || sudo sh -c 'echo 0 > /sys/devices/system/cpu/cpufreq/boost'
```

- **Pin server and load-generator processes to chosen cores**: keep server on dedicated P-cores and run `wrk` on separate cores to avoid contention. Use `taskset` and optionally `chrt` for fixed scheduling priority.

```bash
# start server pinned to P-cores (example cores 0-3)
sudo chrt -f 5 taskset -c 0-3 /path/to/aeronet-bench-server --port 8080 &

# run wrk pinned to other cores (example cores 4-5)
taskset -c 4-5 ./wrk -t2 -c100 -d30s -s lua/mixed_workload.lua http://127.0.0.1:8080/
```

- **Optional: use cpusets / isolcpus for stronger isolation**:

```bash
# Using cset (if installed)
sudo cset shield --cpu 0-3 --kthread=on
# then run server inside the shield or pin explicitly with taskset
```

Or add `isolcpus=` kernel parameter at boot for permanent isolation (requires reboot).

- **Warm the CPU and verify steady frequencies**: run a short CPU warmup to reach steady frequency/thermal conditions, then verify.

```bash
# warm P-cores for ~15s
taskset -c 0-3 stress-ng --cpu 4 --timeout 15s

# verify current frequencies (or use turbostat if available)
watch -n1 "for c in /sys/devices/system/cpu/cpu[0-9]*; do printf '%s %s\n' "${c##*/}" "$(cat $c/cpufreq/scaling_cur_freq 2>/dev/null || echo NA)"; done | head -n 12"

# or use turbostat
sudo turbostat --interval 1
```

- **Run the benchmark**: after governor/turbo/pinning/warmup are applied.

```bash
# Example full sequence
sudo cpupower frequency-set -g performance
sudo sh -c 'echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo' || true
sudo chrt -f 5 taskset -c 0-3 /path/to/aeronet-bench-server --port 8080 &
taskset -c 0-3 stress-ng --cpu 4 --timeout 15s
taskset -c 4-5 ./wrk -t2 -c200 -d30s -s lua/mixed_workload.lua http://127.0.0.1:8080/
```

Notes:

- Keep `wrk` off the same cores as the server to avoid CPU contention. Dedicate at least one core for `wrk` threads.
- When it starts the servers itself, `run_benchmarks.py` **does this pinning automatically** (Linux + `taskset`): the server is pinned to the first `--threads` cores and the load generator (wrk/h2load) to the cores just after it, so they never share a core. It auto-disables when the box is too small to split them (e.g. a 2-core runner running `--threads 2`); disable it explicitly with `--no-cpu-pin`. Manual pinning as shown above is only needed if you start the server yourself.
- Disabling turbo will reduce peak throughput but increases repeatability. Toggle turbo back after measurements if desired.
- If you want automation, consider a small wrapper script that sets governor, pins processes, warms, runs the bench, and restores settings afterwards.

## WebSocket Benchmarks

A separate orchestrator (`run_ws_benchmarks.py`) drives the WebSocket scenarios with `ws-loadgen`, a native
epoll load generator built from [`ws_loadgen.cpp`](ws_loadgen.cpp) (Linux, target `ws-loadgen`; zlib enables
its permessage-deflate support). Each connection performs the handshake, then keeps `--pipeline-depth`
messages in flight, answering every echo / pong with the next message. Doing the minimum per message (one
masked frame write, one frame parse), it costs about as much CPU per message as an efficient server (around
4 us per 128-byte echo, system calls included), so its 3 threads per server thread leave ample headroom.
[k6](https://k6.io/) (about 27 us of CPU per message: it cannot saturate the servers on small machines) remains
available with `--tool k6`, and [websocket-bench](https://github.com/matttomasetti/websocket-bench) optionally
adds a raw throughput run.

The server and the load generator are pinned to disjoint CPUs and the server CPU utilization is reported for
every run, as for the HTTP benchmarks (see [Saturation checks](#saturation-checks)). `ws-loadgen` also reports
its own CPU time over the measurement window (warmup excluded).

### WS Scenarios

| Scenario | ws-loadgen arguments | k6 script | Description |
| ---------- | -------- | -------- | ------------- |
| echo-small | `--mode echo --payload-size 128` | `k6/ws_echo_small.js` | 128 B text echo |
| echo-medium | `--mode echo --payload-size 2048` | `k6/ws_echo_medium.js` | 2 KiB text echo |
| echo-large | `--mode echo --payload-size 65536 --binary` | `k6/ws_echo_large.js` | 64 KiB binary echo (one message in flight per connection) |
| mix | `--mode mix` | `k6/ws_mix_text_binary.js` | Alternating 256 B text and 512 B binary messages |
| ping-pong | `--mode ping` | `k6/ws_ping_pong.js` | Control-frame round trips |
| churn | `--mode churn` | `k6/ws_churn.js` | Connect, send one message, close handshake, reconnect (sessions/s) |
| compression | `--mode echo --json-payload --compress` | `k6/ws_compression.js` | Compressible JSON over permessage-deflate (`/ws-compressed`, aeronet and uWebSockets only) |

Latencies are message round trips, or session durations (connect to close) for `churn`.

### Running WebSocket Benchmarks

```bash
# Full suite - all servers, all scenarios
./run_ws_benchmarks.py

# Quick smoke test (5s, 10 connections)
./run_ws_benchmarks.py --smoke

# Specific servers/scenarios
./run_ws_benchmarks.py --server aeronet,uwebsockets --scenario echo-small,churn

# Server threads, load generator threads (default: automatic), connections, in-flight messages
./run_ws_benchmarks.py --threads 1 --loadgen-threads 3 --vus 50 --pipeline-depth 1 --duration 30s

# k6 instead of ws-loadgen, plus a websocket-bench raw throughput run
./run_ws_benchmarks.py --tool k6 --websocket-bench

# The load generator directly
./ws-loadgen --port 8080 --path /ws-uncompressed -c 50 -t 3 -d 10s --mode echo --payload-size 128

# Render HTML report from a JSON run (interactive charts)
python3 ./render_benchmarks_html.py --input ./ws-results/ws_benchmark_YYYYMMDD_HHMMSS.json --output ./ws-results/ws_benchmark_YYYYMMDD_HHMMSS.html
```

Each `run_ws_benchmarks.py` execution automatically generates:

- text summary (`ws_benchmark_*.txt`)
- machine-readable JSON (`ws_benchmark_*.json`), with the server and load generator CPU utilization per run
- HTML report with charts (`ws_benchmark_*.html`)

### WS Server Support

| Server | Port | `/ws-uncompressed` | `/ws-compressed` |
| -------- | ------ | ----------------- | ----------------- |
| aeronet | 8080 | Yes | Yes |
| drogon | 8081 | Yes | No |
| uwebsockets | 8088 | Yes | Yes |
| beast | 8089 | Yes | No |

## Adding New Servers

1. Create a new server file (e.g., `newserver_server.cpp`)
2. Implement all standard endpoints
3. Add build rules to `CMakeLists.txt`
4. Register in `run_benchmarks.py`

## Future Work

- [x] Add HTTP/2 scenarios (h2load) - h2c and h2-tls modes via `--protocol`
- [x] Add TLS benchmarks - h2-tls mode, self-signed certs generated at runtime
- [ ] Integrate with CI for regression detection
- [x] WebSocket scenarios (ws-loadgen or k6, + websocket-bench, see above)
- [ ] Add streaming/chunked benchmark when comparable APIs exist across frameworks
- [ ] Add automatic compression / decompression benchmarks for frameworks that support it
