# Scripted Server Benchmarks

This directory contains standalone HTTP server executables and `wrk` benchmark scripts to compare **aeronet** against other high-performance HTTP frameworks, possibily implemented in different programming languages.

## Overview

Unlike Google Benchmark (which measures internal latencies), this setup uses **external load generators** to measure real-world throughput and latency under stress. The primary tool is [`wrk`](https://github.com/wg/wrk), a modern HTTP benchmarking tool with LuaJIT scripting support.

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

### 7. Router Stress Test (`routing_stress.lua`)

Tests router lookup performance with large route tables (1000+ routes).

```bash
# Start server with many routes
./aeronet-bench-server --routes 1000 &

# Run benchmark
wrk -t4 -c100 -d30s -s lua/routing_stress.lua http://127.0.0.1:8080/r0
```

## Running the Full Benchmark Suite

Use the Python runner to benchmark all servers and scenarios:

```bash
run_benchmarks.py
```

### Options

```bash
run_benchmarks.py --threads 4        # Number of wrk threads (default: nproc/4)
run_benchmarks.py --connections 100  # Number of wrk connections (default: 100)
run_benchmarks.py --duration 30s     # Benchmark duration per scenario
run_benchmarks.py --warmup 5s        # Warmup duration before each run
run_benchmarks.py --server aeronet   # Only benchmark specific server(s)
run_benchmarks.py --scenario headers # Only run specific scenario(s)
run_benchmarks.py --output results/  # Output directory for result artifacts

# Run multiple values (comma-separated)
run_benchmarks.py --server aeronet,python --scenario headers,body,routing

# Available scenarios: headers, body, static, cpu, mixed, files, routing, tls
```

## Server Implementations

All servers implement identical endpoints:

| Endpoint | Method | Description |
|----------|--------|-------------|
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

### Supported Servers

| Server | Language | File | Notes |
|--------|----------|------|-------|
| aeronet | C++ | `aeronet_server.cpp` | Primary benchmark target |
| drogon | C++ | `drogon_server.cpp` | Popular C++ async framework |
| pistache | C++ | `pistache_server.cpp` | REST framework for C++ |
| crow | C++ | `crow_server.cpp` | Header-only C++ microframework |
| rust | Rust | `rust_server/` | axum async framework |
| undertow | Java | `undertow_server/UndertowBenchServer.java` | High-perf Java NIO server |
| go | Go | `go_server.go` | Standard library net/http |
| python | Python | `python_server.py` | uvicorn + starlette (async) |

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
4. **Multiple runs**: Take the median of 3-5 runs
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
- Disabling turbo will reduce peak throughput but increases repeatability. Toggle turbo back after measurements if desired.
- If `run_benchmarks.py` starts servers for you, prefer starting the server manually pinned (as above) and point the runner at the running server to ensure pinning takes effect.
- If you want automation, consider a small wrapper script that sets governor, pins processes, warms, runs the bench, and restores settings afterwards.

## Adding New Servers

1. Create a new server file (e.g., `newserver_server.cpp`)
2. Implement all standard endpoints
3. Add build rules to `CMakeLists.txt`
4. Register in `run_benchmarks.py`

## Future Work

- [ ] Add HTTP/2 scenarios (h2load)
- [ ] Add TLS benchmarks
- [ ] Integrate with CI for regression detection
- [ ] WebSocket scenarios
- [ ] Add streaming/chunked benchmark when comparable APIs exist across frameworks
- [ ] Add automatic compression / decompression benchmarks for frameworks that support it
