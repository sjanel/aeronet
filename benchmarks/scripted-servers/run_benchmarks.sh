#!/bin/bash
#
# run_benchmarks.sh - Orchestrates wrk benchmarks against multiple HTTP servers

set -e

# Force C locale for consistent number formatting
export LC_NUMERIC=C

# Default configuration
THREADS=${BENCH_THREADS:-$(( $(nproc) / 2 ))}
CONNECTIONS=${BENCH_CONNECTIONS:-100}
DURATION=${BENCH_DURATION:-30s}
WARMUP=${BENCH_WARMUP:-5s}
OUTPUT_DIR=${BENCH_OUTPUT:-./results}
SERVER_FILTER="all"
SCENARIO_FILTER="all"

# Print help text
print_help() {
  cat <<'USAGE'
Usage: run_benchmarks.sh [options]

Options:
  --threads N        Number of wrk threads (default: nproc/2 or $BENCH_THREADS)
  --connections N    Number of connections (default: $CONNECTIONS)
  --duration Ns      Test duration (default: $DURATION)
  --warmup Ns        Warmup duration before each test (default: $WARMUP)
  --output DIR       Output directory for results (default: $OUTPUT_DIR)
  --server NAME      Only benchmark specific server. Comma-separated list allowed.
                     Valid names: aeronet, drogon, pistache, undertow, go, python, rust, all
  --scenario NAME    Only run specific scenario. Comma-separated list allowed.
                     Valid values: headers, body, static, cpu, mixed, files, routing, all
  --help             Show this help and exit

Examples:
  # Run all scenarios against all available servers with defaults
  ./run_benchmarks.sh

  # Run static scenario only on aeronet and pistache
  ./run_benchmarks.sh --server aeronet,pistache --scenario static --duration 10s

  # Run routing stress test
  ./run_benchmarks.sh --scenario routing

Notes:
  - Environment variables can override defaults: BENCH_THREADS, BENCH_CONNECTIONS,
    BENCH_DURATION, BENCH_WARMUP, BENCH_OUTPUT
  - The script will detect build or source directories to find server binaries.
  - Static file benchmark requires setup: ./setup_bench_resources.sh
USAGE
}

# Server configurations
declare -A SERVER_PORTS
SERVER_PORTS[aeronet]=8080
SERVER_PORTS[drogon]=8081
SERVER_PORTS[pistache]=8085
SERVER_PORTS[undertow]=8082
SERVER_PORTS[go]=8083
SERVER_PORTS[python]=8084
SERVER_PORTS[rust]=8086

# Find build directories
find_build_dir() {
  local candidates=(
    "../../build-release/benchmarks/scripted-servers"
    "../../build/benchmarks/scripted-servers"
    "../../../build-release/benchmarks/scripted-servers"
    "../../../build/benchmarks/scripted-servers"
    "."
  )
  for dir in "${candidates[@]}"; do
    if [ -d "$dir" ]; then
      echo "$dir"
      return
    fi
  done
  echo "."
}

BUILD_DIR=$(find_build_dir)

declare -A SERVER_BINS
SERVER_BINS[aeronet]="$BUILD_DIR/aeronet-bench-server"
SERVER_BINS[drogon]="$BUILD_DIR/drogon-bench-server"
SERVER_BINS[pistache]="$BUILD_DIR/pistache-bench-server"
SERVER_BINS[undertow]="java"
SERVER_BINS[go]=""  # Will be set after build
SERVER_BINS[python]="python3"
SERVER_BINS[rust]=""  # Will be set after build

# Server-specific start commands (for interpreted languages)
declare -A SERVER_CMDS
SERVER_CMDS[python]="python_server.py"

# Scenario definitions
declare -A SCENARIOS
SCENARIOS[headers]="lua/headers_stress.lua"
SCENARIOS[body]="lua/large_body.lua"
SCENARIOS[static]="lua/static_routes.lua"
SCENARIOS[cpu]="lua/cpu_bound.lua"
SCENARIOS[mixed]="lua/mixed_workload.lua"
SCENARIOS[files]="lua/static_files.lua"
SCENARIOS[routing]="lua/routing_stress.lua"
SCENARIOS[tls]="lua/tls_handshake.lua"

# Store results for table output
declare -A RESULTS_RPS      # req/sec
declare -A RESULTS_LATENCY  # avg latency
declare -A RESULTS_TRANSFER # transfer/sec

# Parse command line arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    --threads)
      THREADS="$2"
      shift 2
      ;;
    --connections)
      CONNECTIONS="$2"
      shift 2
      ;;
    --duration)
      DURATION="$2"
      shift 2
      ;;
    --warmup)
      WARMUP="$2"
      shift 2
      ;;
    --output)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --server)
      SERVER_FILTER="$2"
      shift 2
      ;;
    --scenario)
      SCENARIO_FILTER="$2"
      shift 2
      ;;
    --help)
      print_help
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      exit 1
      ;;
  esac
done

# Ensure minimum threads
if [ "$THREADS" -lt 1 ]; then
  THREADS=1
fi

echo "=========================================="
echo "       HTTP Server Benchmark Suite        "
echo "=========================================="
echo "Configuration:"
echo "  Threads:     $THREADS"
echo "  Connections: $CONNECTIONS"
echo "  Duration:    $DURATION"
echo "  Warmup:      $WARMUP"
echo "  Output:      $OUTPUT_DIR"
echo "  Server:      $SERVER_FILTER"
echo "  Scenario:    $SCENARIO_FILTER"
echo "=========================================="
echo ""

# Create output directory
mkdir -p "$OUTPUT_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_FILE="$OUTPUT_DIR/benchmark_${TIMESTAMP}.txt"

# Check for wrk
# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Try to detect repository source directory so we can fall back to the
# source `benchmarks/scripted-servers` when running from build directories.
if repo_root=$(git rev-parse --show-toplevel 2>/dev/null); then
  REPO_SCRIPT_DIR="$repo_root/benchmarks/scripted-servers"
else
  # Fallback guess: go up to repo root relative to build dir
  REPO_SCRIPT_DIR="$(cd "$SCRIPT_DIR/.." && cd .. && pwd)/benchmarks/scripted-servers"
fi

STATUS_URL_SCHEME="http"
STATUS_USE_INSECURE=0

# Function to build Go server if needed
build_go_server() {
  if [ -x "$SCRIPT_DIR/go-bench-server" ]; then
    return 0
  fi
  
  if ! command -v go &> /dev/null; then
    return 1
  fi
  
  if [ ! -f "$SCRIPT_DIR/go_server.go" ]; then
    # Try fallback to repository source directory
    if [ -f "$REPO_SCRIPT_DIR/go_server.go" ]; then
      echo "Found go_server.go in repo source dir; building from source dir"
      (cd "$REPO_SCRIPT_DIR" && go build -o go-bench-server go_server.go) || return 1
      # copy binary to script dir for consistency
      cp "$REPO_SCRIPT_DIR/go-bench-server" "$SCRIPT_DIR/" || true
      return 0
    fi
    return 1
  fi
  
  echo "Building Go server..."
  (cd "$SCRIPT_DIR" && go build -o go-bench-server go_server.go) || return 1
  return 0
}

# Function to build Undertow server if needed
build_undertow_server() {
  if [ -f "$SCRIPT_DIR/undertow_server/UndertowBenchServer.class" ]; then
    return 0
  fi
  
  if ! command -v javac &> /dev/null; then
    return 1
  fi
  
  if [ ! -f "$SCRIPT_DIR/undertow_server/UndertowBenchServer.java" ]; then
    # Fallback to repo source dir
    if [ -f "$REPO_SCRIPT_DIR/undertow_server/UndertowBenchServer.java" ]; then
      echo "Found UndertowBenchServer.java in repo source dir; using that path"
      mkdir -p "$SCRIPT_DIR/undertow_server"
      cp "$REPO_SCRIPT_DIR/undertow_server/UndertowBenchServer.java" "$SCRIPT_DIR/undertow_server/" || true
    else
      return 1
    fi
  fi
  
  # Check if undertow jars exist
  if ! ls "$SCRIPT_DIR"/undertow_server/*.jar &> /dev/null; then
    echo "Undertow JARs not found. Attempting to download required JARs into undertow_server/"
    mkdir -p "$SCRIPT_DIR"/undertow_server
    pushd "$SCRIPT_DIR"/undertow_server > /dev/null || return 1
    # List of jars to download (best-effort versions known to work)
    jars=(
      "https://repo1.maven.org/maven2/io/undertow/undertow-core/2.3.10.Final/undertow-core-2.3.10.Final.jar"
      "https://repo1.maven.org/maven2/org/jboss/xnio/xnio-api/3.8.8.Final/xnio-api-3.8.8.Final.jar"
      "https://repo1.maven.org/maven2/org/jboss/xnio/xnio-nio/3.8.8.Final/xnio-nio-3.8.8.Final.jar"
      "https://repo1.maven.org/maven2/org/jboss/logging/jboss-logging/3.5.0.Final/jboss-logging-3.5.0.Final.jar"
      "https://repo1.maven.org/maven2/org/wildfly/common/wildfly-common/1.6.0.Final/wildfly-common-1.6.0.Final.jar"
      "https://repo1.maven.org/maven2/org/jboss/threads/jboss-threads/3.4.0.Final/jboss-threads-3.4.0.Final.jar"
    )
    for u in "${jars[@]}"; do
      fname=$(basename "$u")
      if [ ! -f "$fname" ]; then
        echo "  Downloading $fname..."
        curl -sS -O "$u" || true
      fi
    done
    popd > /dev/null || return 1
    # verify
    if ! ls "$SCRIPT_DIR"/undertow_server/*.jar &> /dev/null; then
      echo "ERROR: Failed to obtain Undertow JARs. Please download them manually into undertow_server/."
      return 1
    fi
  fi
  
  echo "Building Undertow server..."
  # Use classpath which includes current dir and all jars; javac accepts colon-separated list
  (cd "$SCRIPT_DIR/undertow_server" && javac -cp ".:*" UndertowBenchServer.java) || return 1
  return 0
}

# Function to build Rust server if needed
build_rust_server() {
  if ! command -v cargo &> /dev/null; then
    return 1
  fi
  
  local rust_dir=""
  if [ -f "$SCRIPT_DIR/rust_server/Cargo.toml" ]; then
    rust_dir="$SCRIPT_DIR/rust_server"
  elif [ -f "$REPO_SCRIPT_DIR/rust_server/Cargo.toml" ]; then
    rust_dir="$REPO_SCRIPT_DIR/rust_server"
  else
    return 1
  fi
  
  echo "Building Rust server (always rebuild to pick up workspace changes)..."
  (cd "$rust_dir" && cargo build --release) || return 1
  
  # Copy binary to script dir if built from repo dir
  if [ "$rust_dir" = "$REPO_SCRIPT_DIR/rust_server" ] && [ "$SCRIPT_DIR" != "$REPO_SCRIPT_DIR" ]; then
    mkdir -p "$SCRIPT_DIR/rust_server/target/release"
    cp "$rust_dir/target/release/rust-bench-server" "$SCRIPT_DIR/rust_server/target/release/" || true
  fi
  
  return 0
}

# Function to check if server binary/runtime is available
check_server_available() {
  local name=$1
  local bin=${SERVER_BINS[$name]}
  
  case $name in
    aeronet|drogon|pistache)
      [ -x "$bin" ]
      ;;
    go)
      # Try to build, then check for binary
      build_go_server
      if [ -x "$SCRIPT_DIR/go-bench-server" ]; then
        return 0
      elif [ -x "$REPO_SCRIPT_DIR/go-bench-server" ]; then
        return 0
      fi
      return 1
      ;;
    undertow)
      command -v java &> /dev/null && build_undertow_server
      ;;
    python)
      if ! command -v python3 &> /dev/null; then
        return 1
      fi
      python3 -c "import starlette, uvicorn" &> /dev/null 2>&1
      ;;
    rust)
      build_rust_server
      if [ -x "$SCRIPT_DIR/rust_server/target/release/rust-bench-server" ]; then
        return 0
      elif [ -x "$REPO_SCRIPT_DIR/rust_server/target/release/rust-bench-server" ]; then
        return 0
      fi
      return 1
      ;;
    *)
      return 1
      ;;
  esac
}

# Function to start a server
start_server() {
  local name=$1
  shift
  local extra_args=("$@")  # Additional server arguments
  local port=${SERVER_PORTS[$name]}
  local bin=${SERVER_BINS[$name]}
  local cmd=${SERVER_CMDS[$name]:-""}
  
  if ! check_server_available "$name"; then
    echo "WARNING: Server $name not available (skipping)"
    return 1
  fi
  
  echo "Starting $name server on port $port..."
  # Ensure logs directory exists
  mkdir -p "$SCRIPT_DIR/logs"
  local log_file="$SCRIPT_DIR/logs/${name}.log"

  # Check if port is already in use
  if ss -ltn "sport = :$port" | grep -q LISTEN; then
    echo "ERROR: Port $port already in use; check $log_file for details or kill the conflicting process"
    return 1
  fi
  
  case $name in
    aeronet|drogon|pistache)
      BENCH_PORT=$port BENCH_THREADS=$THREADS "$bin" "${extra_args[@]}" >"$log_file" 2>&1 &
      ;;
    go)
      local go_bin=""
      if [ -x "$SCRIPT_DIR/go-bench-server" ]; then
        go_bin="$SCRIPT_DIR/go-bench-server"
      elif [ -x "$REPO_SCRIPT_DIR/go-bench-server" ]; then
        go_bin="$REPO_SCRIPT_DIR/go-bench-server"
      fi
      BENCH_PORT=$port BENCH_THREADS=$THREADS "$go_bin" "${extra_args[@]}" >"$log_file" 2>&1 &
      ;;
    undertow)
      local classpath=$(ls "$SCRIPT_DIR"/undertow_server/*.jar 2>/dev/null | tr '\n' ':')
      # Start Undertow in its own session so we can kill the whole group later
      pushd "$SCRIPT_DIR/undertow_server" > /dev/null || return 1
      setsid env BENCH_PORT=$port BENCH_THREADS=$THREADS java -cp ".:$classpath" UndertowBenchServer "${extra_args[@]}" >"$log_file" 2>&1 &
      popd > /dev/null || true
      ;;
    python)
      # Prefer script in SCRIPT_DIR, fall back to source copy in repository
      local python_script=""
      local python_cwd=""
      if [ -f "$SCRIPT_DIR/$cmd" ]; then
        python_script="$SCRIPT_DIR/$cmd"
        python_cwd="$SCRIPT_DIR"
      elif [ -f "$REPO_SCRIPT_DIR/$cmd" ]; then
        python_script="$REPO_SCRIPT_DIR/$cmd"
        python_cwd="$REPO_SCRIPT_DIR"
      else
        # try some reasonable relative fallbacks
        for p in "$SCRIPT_DIR/../../../benchmarks/scripted-servers" "$SCRIPT_DIR/../../benchmarks/scripted-servers" "$SCRIPT_DIR/../benchmarks/scripted-servers"; do
          if [ -f "$p/$cmd" ]; then
            python_script="$p/$cmd"
            python_cwd="$p"
            break
          fi
        done
      fi

      if [ -z "$python_script" ]; then
        echo "ERROR: python_server.py not found in build or source directories; skipping"
        return 1
      fi

      # Start Python/uvicorn in its own session so workers/children are grouped
      pushd "$python_cwd" > /dev/null || return 1
      setsid env BENCH_PORT=$port BENCH_THREADS=$THREADS python3 "$python_script" "${extra_args[@]}" >"$log_file" 2>&1 &
      popd > /dev/null || true
      ;;
    rust)
      local rust_bin=""
      if [ -x "$SCRIPT_DIR/rust_server/target/release/rust-bench-server" ]; then
        rust_bin="$SCRIPT_DIR/rust_server/target/release/rust-bench-server"
      elif [ -x "$REPO_SCRIPT_DIR/rust_server/target/release/rust-bench-server" ]; then
        rust_bin="$REPO_SCRIPT_DIR/rust_server/target/release/rust-bench-server"
      fi
      
      if [ -z "$rust_bin" ]; then
        echo "ERROR: Rust server binary not found"
        return 1
      fi
      
      BENCH_PORT=$port BENCH_THREADS=$THREADS "$rust_bin" "${extra_args[@]}" >"$log_file" 2>&1 &
      ;;
  esac
  
  local pid=$!
  echo $pid > "/tmp/bench_${name}.pid"
  
  # Wait for server to be ready
  local retries=50
  local scheme=${STATUS_URL_SCHEME:-http}
  local use_insecure=${STATUS_USE_INSECURE:-0}
  local status_url="${scheme}://127.0.0.1:$port/status"
  local curl_cmd=("curl" "-s")
  if [ "$use_insecure" -ne 0 ]; then
    curl_cmd+=("-k")
  fi
  curl_cmd+=("$status_url")
  while ! "${curl_cmd[@]}" > /dev/null 2>&1; do
    retries=$((retries - 1))
    if [ $retries -le 0 ]; then
      echo "ERROR: Server $name failed to start"
      kill $pid 2>/dev/null || true
      return 1
    fi
    sleep 0.2
  done
  
  echo "$name server ready (PID: $pid)"
  return 0
}

# Function to stop a server
stop_server() {
  local name=$1
  local pidfile="/tmp/bench_${name}.pid"
  local port=${SERVER_PORTS[$name]}
  local stopping_dir="/tmp/bench_${name}.stopping.lock"

  # Atomically create a lockdir; if it exists another stopper is running
  if ! mkdir "$stopping_dir" 2>/dev/null; then
    return 0
  fi

  if [ ! -f "$pidfile" ]; then
    rmdir "$stopping_dir" 2>/dev/null || true
    return 0
  fi
  local pid
  pid=$(cat "$pidfile" 2>/dev/null || true)
  if [ -z "$pid" ]; then
    rm -f "$pidfile" "$stopping"
    return 0
  fi

  # If PID not running, clean up and return
  if ! ps -p $pid > /dev/null 2>&1; then
    rm -f "$pidfile" "$stopping"
    return 0
  fi

  echo "Stopping $name server (PID: $pid)..."

  # First try a graceful TERM to the process
  kill -TERM $pid 2>/dev/null || true
  sleep 0.5

  # Terminate child processes of the server PID (best-effort), then the PID itself
  if command -v pgrep > /dev/null 2>&1; then
    # collect descendant PIDs recursively
    descendants=$(pgrep -P $pid || true)
    if [ -n "$descendants" ]; then
      echo "Stopping child processes of PID $pid for $name"
      echo "$descendants" | xargs -r -n1 kill -TERM 2>/dev/null || true
      sleep 0.2
      echo "$descendants" | xargs -r -n1 kill -KILL 2>/dev/null || true
    fi
  fi

  # If process still exists, kill directly
  if ps -p $pid > /dev/null 2>&1; then
    echo "Forcing kill of PID $pid"
    kill -9 $pid 2>/dev/null || true
  fi

  # If port is still in use, find listening PIDs and kill them
  if command -v ss > /dev/null 2>&1; then
    if ss -ltn "sport = :$port" | grep -q LISTEN; then
      echo "Port $port still in use; killing any processes listening on the port"
      if command -v lsof > /dev/null 2>&1; then
        lsof -t -iTCP:$port -sTCP:LISTEN | xargs -r -n1 kill -9 2>/dev/null || true
      else
        ss -ltnp "sport = :$port" 2>/dev/null | awk -F',' '/pid=/ { for(i=1;i<=NF;i++) if ($i ~ /pid=/) { sub("pid=","",$i); split($i,a," "); print a[1] } }' | xargs -r -n1 kill -9 2>/dev/null || true
      fi
    fi
  fi

  rm -f "$pidfile"
  rmdir "$stopping_dir" 2>/dev/null || true
}

# Function to run a benchmark
run_benchmark() {
  local server=$1
  local scenario=$2
  local lua_script=$3
  local port=${SERVER_PORTS[$server]}
  local url="http://127.0.0.1:$port"
  
  echo ""
  echo ">>> Running: $server / $scenario"
  echo "    Script: $lua_script"
  echo "    URL: $url"
  
  # Determine endpoint based on scenario
  local endpoint="/"
  case $scenario in
    headers) endpoint="/headers" ;;
    body) endpoint="/echo" ;;
    static) endpoint="/ping" ;;
    cpu) endpoint="/compute" ;;
    mixed) endpoint="/" ;;
    files) endpoint="/static/index.html" ;;
    routing) endpoint="/r0" ;;
    tls) endpoint="/ping" ;;
  esac
  
  # Use HTTPS for TLS scenario
  if [ "$scenario" = "tls" ]; then
    url="https://127.0.0.1:$port"
  fi
  
  # Warmup run (short duration, no output)
  echo "    Warming up ($WARMUP)..."
  wrk -t$THREADS -c$CONNECTIONS -d$WARMUP -s "$SCRIPT_DIR/$lua_script" "$url$endpoint" > /dev/null 2>&1 || true
  
  # Actual benchmark
  echo "    Benchmarking ($DURATION)..."
  local output
  output=$(wrk -t$THREADS -c$CONNECTIONS -d$DURATION -s "$SCRIPT_DIR/$lua_script" "$url$endpoint" 2>&1)
  
  # Parse results for table
  local non2xx=$(echo "$output" | awk -F: '/Non-2xx or 3xx responses/ {gsub(/[[:space:]]*/, "", $2); print $2}')
  if [ -z "$non2xx" ]; then
    non2xx=0
  fi

  local rps="-"
  local latency="-"
  local transfer="-"
  if [ "$non2xx" -eq 0 ]; then
    rps=$(echo "$output" | grep "Requests/sec:" | awk '{print $2}')
    latency=$(echo "$output" | grep "Latency" | head -1 | awk '{print $2}')
    transfer=$(echo "$output" | grep "Transfer/sec:" | awk '{print $2}')
  else
    echo "WARNING: $server / $scenario reported $non2xx non-2xx responses; metrics ignored"
  fi

  # Store results
  local key="${server}:${scenario}"
  RESULTS_RPS[$key]="$rps"
  RESULTS_LATENCY[$key]="$latency"
  RESULTS_TRANSFER[$key]="$transfer"
  
  # Print and save results
  echo "$output"
  echo ""
  echo "=== $server / $scenario ===" >> "$RESULT_FILE"
  echo "$output" >> "$RESULT_FILE"
  echo "" >> "$RESULT_FILE"
}

# Cleanup on exit
cleanup() {
  for server in "${!SERVER_PORTS[@]}"; do
    stop_server "$server"
  done
}
trap cleanup EXIT

# Main benchmark loop
echo "Starting benchmarks..."
echo "Results will be saved to: $RESULT_FILE"
echo ""

# Write header to results file
{
  echo "HTTP Server Benchmark Results"
  echo "=============================="
  echo "Date: $(date)"
  echo "Threads: $THREADS"
  echo "Connections: $CONNECTIONS"
  echo "Duration: $DURATION"
  echo "System: $(uname -a)"
  echo "CPU: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)"
  echo ""
} > "$RESULT_FILE"

# Determine which servers to test
servers_to_test=()
if [ "$SERVER_FILTER" = "all" ]; then
  # Only include servers that are available
  for srv in aeronet drogon pistache undertow go python rust; do
    if check_server_available "$srv"; then
      servers_to_test+=("$srv")
    fi
  done
  if [ ${#servers_to_test[@]} -eq 0 ]; then
    echo "ERROR: No servers available to test"
    exit 1
  fi
else
  # Allow comma-separated list
  IFS=',' read -ra servers_to_test <<< "$SERVER_FILTER"
fi

# Determine which scenarios to test
scenarios_to_test=()
if [ "$SCENARIO_FILTER" = "all" ]; then
  # TODO: add tls
  # scenarios_to_test=("headers" "body" "static" "cpu" "mixed" "files" "routing" "tls")
  scenarios_to_test=("headers" "body" "static" "cpu" "mixed" "files" "routing")
else
  # Allow comma-separated list
  IFS=',' read -ra scenarios_to_test <<< "$SCENARIO_FILTER"
fi

# Check if static files scenario is included - set up resources if needed
needs_static_files=false
needs_tls=false
for scenario in "${scenarios_to_test[@]}"; do
  if [ "$scenario" = "files" ]; then
    needs_static_files=true
  fi
  if [ "$scenario" = "tls" ]; then
    needs_tls=true
  fi
done

if [ "$needs_static_files" = true ] || [ "$needs_tls" = true ]; then
  STATIC_DIR="$SCRIPT_DIR/static"
  CERTS_DIR="$SCRIPT_DIR/certs"
  if [ ! -d "$STATIC_DIR" ] || [ ! -f "$STATIC_DIR/index.html" ] || [ ! -f "$CERTS_DIR/server.crt" ]; then
    echo "Setting up benchmark resources (static files and/or TLS certs)..."
    if [ -x "$SCRIPT_DIR/setup_bench_resources.sh" ]; then
      "$SCRIPT_DIR/setup_bench_resources.sh" "$SCRIPT_DIR"
    elif [ -x "$REPO_SCRIPT_DIR/setup_bench_resources.sh" ]; then
      "$REPO_SCRIPT_DIR/setup_bench_resources.sh" "$SCRIPT_DIR"
    else
      echo "WARNING: setup_bench_resources.sh not found - 'files' and 'tls' scenarios may fail"
    fi
  fi
fi

# Helper function to get extra server args for a scenario
get_server_args() {
  local server=$1
  local scenario=$2
  local args=()
  
  case $scenario in
    files)
      # All frameworks use --static for static file directory
      if [ -d "$SCRIPT_DIR/static" ]; then
        args+=("--static" "$SCRIPT_DIR/static")
      fi
      ;;
    routing)
      # All frameworks use --routes for route count
      args+=("--routes" "1000")
      ;;
    tls)
      # TLS is only supported by aeronet for now
      if [ "$server" = "aeronet" ]; then
        if [ -f "$SCRIPT_DIR/certs/server.crt" ] && [ -f "$SCRIPT_DIR/certs/server.key" ]; then
          args+=("--tls" "--cert" "$SCRIPT_DIR/certs/server.crt" "--key" "$SCRIPT_DIR/certs/server.key")
        fi
      fi
      ;;
  esac
  
  echo "${args[@]}"
}

# Check if scenario needs server restart (different config)
needs_restart() {
  local scenario=$1
  case $scenario in
    files|routing|tls) return 0 ;;
    *) return 1 ;;
  esac
}

# Run benchmarks for each server
for server in "${servers_to_test[@]}"; do
  echo ""
  echo "=========================================="
  echo "Testing: $server"
  echo "=========================================="
  
  # Group scenarios by whether they need special server config
  normal_scenarios=()
  special_scenarios=()
  for scenario in "${scenarios_to_test[@]}"; do
    if needs_restart "$scenario"; then
      special_scenarios+=("$scenario")
    else
      normal_scenarios+=("$scenario")
    fi
  done
  
  # Run normal scenarios (server started once)
  if [ ${#normal_scenarios[@]} -gt 0 ]; then
    if ! start_server "$server"; then
      echo "Skipping $server (failed to start)"
      continue
    fi
    
    for scenario in "${normal_scenarios[@]}"; do
      lua_script=${SCENARIOS[$scenario]}
      if [ -z "$lua_script" ]; then
        echo "WARNING: Unknown scenario: $scenario"
        continue
      fi
      
      if [ ! -f "$SCRIPT_DIR/$lua_script" ]; then
        echo "WARNING: Lua script not found: $lua_script"
        continue
      fi
      
      run_benchmark "$server" "$scenario" "$lua_script"
    done
    
    stop_server "$server"
    sleep 1
  fi
  
  # Run special scenarios (server restarted with special args)
  for scenario in "${special_scenarios[@]}"; do
    if [ "$scenario" = "tls" ]; then
      STATUS_URL_SCHEME="https"
      STATUS_USE_INSECURE=1
    else
      STATUS_URL_SCHEME="http"
      STATUS_USE_INSECURE=0
    fi
    # Skip TLS for non-aeronet servers (only aeronet supports TLS in benchmarks)
    if [ "$scenario" = "tls" ] && [ "$server" != "aeronet" ]; then
      echo "Skipping TLS scenario for $server (not supported)"
      continue
    fi
    
    lua_script=${SCENARIOS[$scenario]}
    if [ -z "$lua_script" ]; then
      echo "WARNING: Unknown scenario: $scenario"
      continue
    fi
    
    if [ ! -f "$SCRIPT_DIR/$lua_script" ]; then
      echo "WARNING: Lua script not found: $lua_script"
      continue
    fi
    
    # Get scenario-specific args
    extra_args=()
    read -ra extra_args <<< "$(get_server_args "$server" "$scenario")"
    
    echo "Starting $server with extra args: ${extra_args[*]:-none}"
    if ! start_server "$server" "${extra_args[@]}"; then
      echo "Skipping $server / $scenario (failed to start)"
      continue
    fi
    
    run_benchmark "$server" "$scenario" "$lua_script"
    
    stop_server "$server"
    sleep 1
  done
done

echo ""
echo "=========================================="
echo "Benchmarks complete!"
echo "Results saved to: $RESULT_FILE"
echo "=========================================="

# Function to format number with thousands separator and color
format_rps() {
  local val="$1"
  if [ -z "$val" ] || [ "$val" = "-" ]; then
    echo "-"
    return
  fi
  # Remove decimal and format with thousands separator
  # Use awk for reliable floating-point rounding across shells/locales
  local int_val
  int_val=$(awk -v v="$val" 'BEGIN { if (v=="-" || v=="") { print "0"; exit } printf("%.0f", v) }' 2>/dev/null || echo "0")
  # Add thousands separator manually
  echo "$int_val" | sed ':a;s/\B[0-9]\{3\}\>/ &/;ta'
}

# Function to find the best result for a scenario (highest req/sec)
find_best_for_scenario() {
  local scenario=$1
  local best_server=""
  local best_val=0
  
  for srv in "${servers_to_test[@]}"; do
    local key="${srv}:${scenario}"
    local val="${RESULTS_RPS[$key]}"
    if [ -n "$val" ] && [ "$val" != "-" ]; then
      local int_val=$(printf "%.0f" "$val" 2>/dev/null || echo "0")
      if [ "$int_val" -gt "$best_val" ]; then
        best_val=$int_val
        best_server=$srv
      fi
    fi
  done
  echo "$best_server"
}

# Generate summary comparison table
print_results_table() {
  echo ""
  
  # Column widths
  local SCEN_W=12
  local CELL_W=14
  local WIN_W=10
  local NUM_SRVS=${#servers_to_test[@]}

  # Compute interior width (characters between outer box borders)
  # interior = SCEN_W+3 + NUM_SRVS*(CELL_W+3) + WIN_W+2
  local INTERIOR_W=$((SCEN_W + 3 + NUM_SRVS * (CELL_W + 3) + WIN_W + 2))

  # Top border
  printf '╔'
  printf '═%.0s' $(seq 1 $INTERIOR_W)
  printf '╗\n'

  # Centered title lines
  local title1="BENCHMARK RESULTS COMPARISON"
  local title2="(Requests/sec - higher is better)"
  for t in "$title1" "$title2"; do
    local len=${#t}
    local left=$(((INTERIOR_W - len) / 2))
    local right=$((INTERIOR_W - len - left))
    printf '║'
    printf "%${left}s" ''
    printf '%s' "$t"
    printf "%${right}s" ''
    printf '║\n'
  done

  # Middle separator
  printf '╠'
  printf '═%.0s' $(seq 1 $INTERIOR_W)
  printf '╣\n'

  # Print header row
  printf "║ %-*s │" "$SCEN_W" "Scenario"
  for srv in "${servers_to_test[@]}"; do
    printf " %-*s │" "$CELL_W" "$srv"
  done
  printf " %-*s ║\n" "$WIN_W" "Winner"
  
  printf '╠'
  printf '═%.0s' $(seq 1 $INTERIOR_W)
  printf '╣\n'
  
  # Print each scenario row
  for scenario in "${scenarios_to_test[@]}"; do
    printf "║ %-12s │" "$scenario"
    
    local best_server=$(find_best_for_scenario "$scenario")
    
    for srv in "${servers_to_test[@]}"; do
      local key="${srv}:${scenario}"
      local val="${RESULTS_RPS[$key]:-"-"}"
      local formatted=$(format_rps "$val")
      # Truncate formatted value to fit inside cell when winner star is printed
      if [ "$srv" = "$best_server" ] && [ -n "$best_server" ]; then
        local maxval=$((CELL_W-2))
        local display=${formatted}
        if [ ${#display} -gt $maxval ]; then
          display=${display:0:$maxval}
        fi
        printf " %-*s" "$maxval" "$display"
        printf " \e[1;32m★\e[0m │"
      else
        printf " %-*s │" "$CELL_W" "$formatted"
      fi
    done
    
    if [ -n "$best_server" ]; then
      printf " %-*s ║\n" "$WIN_W" "$best_server"
    else
      printf " %-*s ║\n" "$WIN_W" "-"
    fi
  done
  
  # Bottom border
  printf '╚'
  printf '═%.0s' $(seq 1 $INTERIOR_W)
  printf '╝\n'
  
  # Print latency table
  echo ""
  # Latency box top border
  printf '╔'
  printf '═%.0s' $(seq 1 $INTERIOR_W)
  printf '╗\n'

  # Latency title lines
  local ltitle1="LATENCY COMPARISON"
  local ltitle2="(Average - lower is better)"
  for t in "$ltitle1" "$ltitle2"; do
    local len=${#t}
    local left=$(((INTERIOR_W - len) / 2))
    local right=$((INTERIOR_W - len - left))
    printf '║'
    printf "%${left}s" ''
    printf '%s' "$t"
    printf "%${right}s" ''
    printf '║\n'
  done

  # Middle separator
  printf '╠'
  printf '═%.0s' $(seq 1 $INTERIOR_W)
  printf '╣\n'
  
  printf "║ %-12s │" "Scenario"
  for srv in "${servers_to_test[@]}"; do
    printf " %-*s │" "$CELL_W" "$srv"
  done
  printf " %-*s ║\n" "$WIN_W" "Best"
  
  printf '╠'
  printf '═%.0s' $(seq 1 $INTERIOR_W)
  printf '╣\n'
  
  for scenario in "${scenarios_to_test[@]}"; do
    printf "║ %-12s │" "$scenario"
    
    # Find best (lowest) latency
    local best_server=""
    local best_val=999999999
    for srv in "${servers_to_test[@]}"; do
      local key="${srv}:${scenario}"
      local val="${RESULTS_LATENCY[$key]}"
      if [ -n "$val" ] && [ "$val" != "-" ]; then
        # Convert to microseconds for comparison (handle us, ms, s)
        local numeric=$(echo "$val" | sed 's/[^0-9.]//g')
        local unit=$(echo "$val" | sed 's/[0-9.]//g')
        local us_val=0
        case "$unit" in
          us) us_val=$(printf "%.0f" "$numeric") ;;
          ms) us_val=$(printf "%.0f" "$(echo "$numeric * 1000" | bc)") ;;
          s)  us_val=$(printf "%.0f" "$(echo "$numeric * 1000000" | bc)") ;;
          *)  us_val=$(printf "%.0f" "$numeric") ;;
        esac
        if [ "$us_val" -lt "$best_val" ] 2>/dev/null; then
          best_val=$us_val
          best_server=$srv
        fi
      fi
    done
    
    for srv in "${servers_to_test[@]}"; do
      local key="${srv}:${scenario}"
      local val="${RESULTS_LATENCY[$key]:-"-"}"
      # For latency, reserve 2 columns for the star when needed
      if [ "$srv" = "$best_server" ] && [ -n "$best_server" ]; then
        local maxval=$((CELL_W-2))
        local display=${val}
        if [ ${#display} -gt $maxval ]; then
          display=${display:0:$maxval}
        fi
        printf " %-*s" "$maxval" "$display"
        printf " \e[1;32m★\e[0m │"
      else
        printf " %-*s │" "$CELL_W" "$val"
      fi
    done
    
    if [ -n "$best_server" ]; then
      printf " %-*s ║\n" "$WIN_W" "$best_server"
    else
      printf " %-*s ║\n" "$WIN_W" "-"
    fi
  done
  
  printf '╚'
  printf '═%.0s' $(seq 1 $INTERIOR_W)
  printf '╝\n'
  
  # Print transfer rate table
  echo ""
  printf '╔'
  printf '═%.0s' $(seq 1 $INTERIOR_W)
  printf '╗\n'

  # Transfer rate title lines
  local ttitle1="TRANSFER RATE COMPARISON"
  local ttitle2="(Data throughput - higher is better)"
  for t in "$ttitle1" "$ttitle2"; do
    local len=${#t}
    local left=$(((INTERIOR_W - len) / 2))
    local right=$((INTERIOR_W - len - left))
    printf '║'
    printf "%${left}s" ''
    printf '%s' "$t"
    printf "%${right}s" ''
    printf '║\n'
  done

  # Middle separator
  printf '╠'
  printf '═%.0s' $(seq 1 $INTERIOR_W)
  printf '╣\n'
  
  printf "║ %-12s │" "Scenario"
  for srv in "${servers_to_test[@]}"; do
    printf " %-*s │" "$CELL_W" "$srv"
  done
  printf " %-*s ║\n" "$WIN_W" "Best"
  
  printf '╠'
  printf '═%.0s' $(seq 1 $INTERIOR_W)
  printf '╣\n'
  
  for scenario in "${scenarios_to_test[@]}"; do
    printf "║ %-12s │" "$scenario"
    
    # Find best (highest) transfer rate
    local best_server=""
    local best_val=0
    for srv in "${servers_to_test[@]}"; do
      local key="${srv}:${scenario}"
      local val="${RESULTS_TRANSFER[$key]}"
      if [ -n "$val" ] && [ "$val" != "-" ]; then
        # Convert to bytes for comparison (handle KB, MB, GB)
        local numeric=$(echo "$val" | sed 's/[^0-9.]//g')
        local unit=$(echo "$val" | sed 's/[0-9.]//g')
        local bytes_val=0
        case "$unit" in
          B)  bytes_val=$(printf "%.0f" "$numeric") ;;
          KB) bytes_val=$(printf "%.0f" "$(echo "$numeric * 1024" | bc)") ;;
          MB) bytes_val=$(printf "%.0f" "$(echo "$numeric * 1048576" | bc)") ;;
          GB) bytes_val=$(printf "%.0f" "$(echo "$numeric * 1073741824" | bc)") ;;
          *)  bytes_val=$(printf "%.0f" "$numeric") ;;
        esac
        if [ "$bytes_val" -gt "$best_val" ] 2>/dev/null; then
          best_val=$bytes_val
          best_server=$srv
        fi
      fi
    done
    
    for srv in "${servers_to_test[@]}"; do
      local key="${srv}:${scenario}"
      local val="${RESULTS_TRANSFER[$key]:-"-"}"
      if [ "$srv" = "$best_server" ] && [ -n "$best_server" ]; then
        local maxval=$((CELL_W-2))
        local display=${val}
        if [ ${#display} -gt $maxval ]; then
          display=${display:0:$maxval}
        fi
        printf " %-*s" "$maxval" "$display"
        printf " \e[1;32m★\e[0m │"
      else
        printf " %-*s │" "$CELL_W" "$val"
      fi
    done
    
    if [ -n "$best_server" ]; then
      printf " %-*s ║\n" "$WIN_W" "$best_server"
    else
      printf " %-*s ║\n" "$WIN_W" "-"
    fi
  done
  
  printf '╚'
  printf '═%.0s' $(seq 1 $INTERIOR_W)
  printf '╝\n'
}

# Print results table if we have results
if [ ${#RESULTS_RPS[@]} -gt 0 ]; then
  print_results_table
  
  # Also save table to file (without colors)
  {
    echo ""
    echo "=== SUMMARY TABLE ==="
    echo ""
    printf "%-12s" "Scenario"
    for srv in "${servers_to_test[@]}"; do
      printf " | %-14s" "$srv"
    done
    echo " | Winner"
    echo "------------|----------------|----------------|----------------|----------------|----------------|--------"
    
    for scenario in "${scenarios_to_test[@]}"; do
      printf "%-12s" "$scenario"
      best_server=$(find_best_for_scenario "$scenario")
      for srv in "${servers_to_test[@]}"; do
        key="${srv}:${scenario}"
        val="${RESULTS_RPS[$key]:-"-"}"
        printf " | %-14s" "$(format_rps "$val")"
      done
      printf " | %s\n" "${best_server:--}"
    done
  } >> "$RESULT_FILE"
fi

echo ""
echo "Legend: ★ = Winner for this scenario"
echo ""