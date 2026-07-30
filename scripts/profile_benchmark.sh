#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)

FREQ=200
EVENT=cycles
CALL_GRAPH=dwarf
DO_BUILD=0
BUILD_DIR="${REPO_ROOT}/build-profile"
OUTPUT_DIR=""
PERF_DATA=""
INPUT_DATA=""
TARGET_PID=""
RECORD_ONLY=0
GENERATE_FLAMEGRAPH=1
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-}"
INSTALL_FLAMEGRAPH=0
OPEN_HOTSPOT=0
HOTSPOT_BIN="${HOTSPOT_BIN:-}"
USE_SUDO=0
COMMAND=()

Usage() {
  cat <<'EOF'
Profile an aeronet benchmark with Linux perf.

Usage:
  scripts/profile_benchmark.sh [options] -- <command> [args...]
  scripts/profile_benchmark.sh [options] --pid <pid>
  scripts/profile_benchmark.sh [options] --input <perf.data>
  scripts/profile_benchmark.sh --build [--build-dir <dir>]

Recording options:
  --pid PID              Attach to a running process instead of launching a command.
  --freq N               Sampling frequency (default: 200).
  --event EVENT          perf event to sample (default: cycles).
  --call-graph MODE      Call graph mode: dwarf, fp, or lbr (default: dwarf).
  --sudo                 Run perf record with sudo. Not supported with --record-only.
  --record-only          Record without post-processing. Intended for benchmark runners.

Artifact options:
  --output-dir DIR       Artifact directory (default: profiles/profile-<timestamp>).
  --data FILE            Exact output perf.data path. Implies its parent output directory.
  --input FILE           Post-process an existing perf.data without recording.
  --no-flamegraph        Do not generate flamegraph.svg.
  --flamegraph-dir DIR   Directory containing stackcollapse-perf.pl and flamegraph.pl.
  --install-flamegraph   Clone FlameGraph into the user cache if it is not installed.
  --hotspot              Open perf.data in Hotspot after recording/post-processing.
  --hotspot-bin FILE     Hotspot executable or AppImage path.

Build options:
  --build                Configure and build benchmarks with debug info and frame pointers.
  --build-dir DIR        Profile build directory (default: build-profile).

Examples:
  scripts/profile_benchmark.sh -- ./build-release/benchmarks/internal/router_bench
  scripts/profile_benchmark.sh --pid "$(pidof aeronet-bench-server)" --hotspot
  scripts/profile_benchmark.sh --input profiles/run/perf.data --install-flamegraph

Hotspot is discovered on PATH and as ~/Downloads/hotspot-*.AppImage.
EOF
}

Die() {
  echo "error: $*" >&2
  exit 2
}

NeedValue() {
  [[ $# -ge 2 ]] || Die "$1 requires a value"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build)
      DO_BUILD=1
      shift
      ;;
    --build-dir)
      NeedValue "$@"
      BUILD_DIR=$2
      shift 2
      ;;
    --freq)
      NeedValue "$@"
      FREQ=$2
      shift 2
      ;;
    --event)
      NeedValue "$@"
      EVENT=$2
      shift 2
      ;;
    --call-graph)
      NeedValue "$@"
      CALL_GRAPH=$2
      shift 2
      ;;
    --output-dir)
      NeedValue "$@"
      OUTPUT_DIR=$2
      shift 2
      ;;
    --data)
      NeedValue "$@"
      PERF_DATA=$2
      shift 2
      ;;
    --input)
      NeedValue "$@"
      INPUT_DATA=$2
      shift 2
      ;;
    --pid)
      NeedValue "$@"
      TARGET_PID=$2
      shift 2
      ;;
    --record-only)
      RECORD_ONLY=1
      shift
      ;;
    --no-flamegraph)
      GENERATE_FLAMEGRAPH=0
      shift
      ;;
    --flamegraph-dir)
      NeedValue "$@"
      FLAMEGRAPH_DIR=$2
      shift 2
      ;;
    --install-flamegraph)
      INSTALL_FLAMEGRAPH=1
      shift
      ;;
    --hotspot)
      OPEN_HOTSPOT=1
      shift
      ;;
    --hotspot-bin)
      NeedValue "$@"
      HOTSPOT_BIN=$2
      shift 2
      ;;
    --sudo)
      USE_SUDO=1
      shift
      ;;
    --)
      shift
      COMMAND=("$@")
      break
      ;;
    -h|--help)
      Usage
      exit 0
      ;;
    *)
      Die "unknown option: $1"
      ;;
  esac
done

[[ "$FREQ" =~ ^[1-9][0-9]*$ ]] || Die "--freq must be a positive integer"
[[ "$CALL_GRAPH" == dwarf || "$CALL_GRAPH" == fp || "$CALL_GRAPH" == lbr ]] \
  || Die "--call-graph must be dwarf, fp, or lbr"
[[ -z "$TARGET_PID" || "$TARGET_PID" =~ ^[1-9][0-9]*$ ]] || Die "--pid must be a positive integer"
if [[ -n "$FLAMEGRAPH_DIR" ]]; then
  [[ -x "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" && -x "${FLAMEGRAPH_DIR}/flamegraph.pl" ]] \
    || Die "FlameGraph scripts not found in: $FLAMEGRAPH_DIR"
fi

if [[ -n "$INPUT_DATA" && ( -n "$TARGET_PID" || ${#COMMAND[@]} -gt 0 ) ]]; then
  Die "--input cannot be combined with --pid or a command"
fi
if [[ -n "$TARGET_PID" && ${#COMMAND[@]} -gt 0 ]]; then
  Die "use either --pid or a command, not both"
fi
if [[ $RECORD_ONLY -eq 1 && -n "$INPUT_DATA" ]]; then
  Die "--record-only cannot be combined with --input"
fi
if [[ $RECORD_ONLY -eq 1 && $USE_SUDO -eq 1 ]]; then
  Die "--sudo is not supported with --record-only; configure perf permissions for scripted benchmarks"
fi

if [[ $DO_BUILD -eq 1 ]]; then
  echo "Configuring profile build in ${BUILD_DIR}..." >&2
  cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja \
    -DAERONET_BUILD_TESTS=OFF \
    -DAERONET_BUILD_EXAMPLES=OFF \
    -DAERONET_BUILD_BENCHMARKS=ON \
    -DAERONET_BENCH_ENABLE_DROGON=OFF \
    -DAERONET_BENCH_ENABLE_OATPP=OFF \
    -DAERONET_BENCH_ENABLE_HTTPLIB=OFF \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -DNDEBUG -fno-omit-frame-pointer" \
    -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g -DNDEBUG -fno-omit-frame-pointer"
  cmake --build "$BUILD_DIR"
  echo "Profile build complete: ${BUILD_DIR}" >&2
fi

if [[ -z "$INPUT_DATA" && -z "$TARGET_PID" && ${#COMMAND[@]} -eq 0 ]]; then
  if [[ $DO_BUILD -eq 1 ]]; then
    exit 0
  fi
  Usage >&2
  exit 2
fi

FindPerf() {
  local candidate
  if [[ -n "${PERF_BIN:-}" && -x "${PERF_BIN}" ]]; then
    printf '%s\n' "$PERF_BIN"
    return
  fi
  if command -v perf >/dev/null 2>&1 && perf --version >/dev/null 2>&1; then
    command -v perf
    return
  fi
  for candidate in /usr/lib/linux-tools-*/perf /usr/lib/linux-tools/*/perf /usr/lib/linux-hwe-*/perf; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return
    fi
  done
  return 1
}

PERF_BIN=$(FindPerf) || {
  echo "error: perf is not installed or its kernel-specific executable is missing." >&2
  echo "Ubuntu/Debian: sudo apt install linux-tools-$(uname -r)" >&2
  exit 1
}

timestamp=$(date +%Y%m%d-%H%M%S)
if [[ -n "$INPUT_DATA" ]]; then
  [[ -f "$INPUT_DATA" ]] || Die "perf data file not found: $INPUT_DATA"
  PERF_DATA=$INPUT_DATA
  if [[ -z "$OUTPUT_DIR" ]]; then
    OUTPUT_DIR=$(dirname "$PERF_DATA")
  fi
else
  if [[ -n "$PERF_DATA" && -z "$OUTPUT_DIR" ]]; then
    OUTPUT_DIR=$(dirname "$PERF_DATA")
  fi
  if [[ -z "$OUTPUT_DIR" ]]; then
    OUTPUT_DIR="${REPO_ROOT}/profiles/profile-${timestamp}"
  fi
  if [[ -z "$PERF_DATA" ]]; then
    PERF_DATA="${OUTPUT_DIR}/perf.data"
  fi
fi
mkdir -p "$OUTPUT_DIR"

PrintPerfPermissionHint() {
  local paranoid="unknown"
  if [[ -r /proc/sys/kernel/perf_event_paranoid ]]; then
    paranoid=$(</proc/sys/kernel/perf_event_paranoid)
  fi
  cat >&2 <<EOF
perf recording failed (kernel.perf_event_paranoid=${paranoid}).
For a one-off command, retry with --sudo. For scripted benchmarks, allow
unprivileged profiling before the run, for example:
  sudo sysctl kernel.perf_event_paranoid=1
To persist it, add 'kernel.perf_event_paranoid=1' to a sysctl.d configuration.
EOF
}

record_status=0
if [[ -z "$INPUT_DATA" ]]; then
  record_command=(
    "$PERF_BIN" record
    --freq "$FREQ"
    --event "$EVENT"
    --call-graph "$CALL_GRAPH"
    --output "$PERF_DATA"
  )
  if [[ -n "$TARGET_PID" ]]; then
    kill -0 "$TARGET_PID" 2>/dev/null || Die "process is not running: $TARGET_PID"
    record_command+=(--pid "$TARGET_PID" --inherit)
    echo "Recording PID ${TARGET_PID} to ${PERF_DATA}..." >&2
  else
    record_command+=(-- "${COMMAND[@]}")
    echo "Recording command to ${PERF_DATA}: ${COMMAND[*]}" >&2
  fi

  if [[ $RECORD_ONLY -eq 1 ]]; then
    exec "${record_command[@]}"
  fi

  if [[ $USE_SUDO -eq 1 ]]; then
    record_command=(sudo -- "${record_command[@]}")
  fi
  set +e
  "${record_command[@]}"
  record_status=$?
  set -e

  if [[ $USE_SUDO -eq 1 && -f "$PERF_DATA" ]]; then
    sudo chown "$(id -u):$(id -g)" "$PERF_DATA"
  fi
  if [[ ! -s "$PERF_DATA" ]]; then
    PrintPerfPermissionHint
    if [[ $record_status -eq 0 ]]; then
      exit 1
    fi
    exit "$record_status"
  fi
  if [[ $record_status -ne 0 ]]; then
    echo "warning: perf record exited with status ${record_status}; processing captured samples." >&2
  fi
fi

PERF_SCRIPT="${OUTPUT_DIR}/perf.script"
echo "Writing ${PERF_SCRIPT}..." >&2
"$PERF_BIN" script --input "$PERF_DATA" > "$PERF_SCRIPT"

FindFlameGraphDir() {
  local candidate
  if [[ -n "$FLAMEGRAPH_DIR" ]]; then
    printf '%s\n' "$FLAMEGRAPH_DIR"
    return
  fi

  local collapse=""
  local render=""
  collapse=$(command -v stackcollapse-perf.pl 2>/dev/null || true)
  render=$(command -v flamegraph.pl 2>/dev/null || true)
  if [[ -n "$collapse" && -n "$render" && $(dirname "$collapse") == "$(dirname "$render")" ]]; then
    dirname "$collapse"
    return
  fi

  for candidate in \
    "${SCRIPT_DIR}/FlameGraph" \
    "${HOME}/FlameGraph" \
    "${XDG_CACHE_HOME:-${HOME}/.cache}/aeronet/FlameGraph" \
    /usr/local/share/FlameGraph \
    /usr/share/FlameGraph \
    /usr/share/flamegraph; do
    if [[ -x "${candidate}/stackcollapse-perf.pl" && -x "${candidate}/flamegraph.pl" ]]; then
      printf '%s\n' "$candidate"
      return
    fi
  done
  return 1
}

if [[ $GENERATE_FLAMEGRAPH -eq 1 ]]; then
  if ! resolved_flamegraph_dir=$(FindFlameGraphDir); then
    if [[ $INSTALL_FLAMEGRAPH -eq 1 ]]; then
      command -v git >/dev/null 2>&1 || Die "git is required by --install-flamegraph"
      resolved_flamegraph_dir="${XDG_CACHE_HOME:-${HOME}/.cache}/aeronet/FlameGraph"
      mkdir -p "$(dirname "$resolved_flamegraph_dir")"
      echo "Installing FlameGraph in ${resolved_flamegraph_dir}..." >&2
      git clone --depth 1 https://github.com/brendangregg/FlameGraph.git "$resolved_flamegraph_dir"
    else
      resolved_flamegraph_dir=""
    fi
  fi

  if [[ -n "$resolved_flamegraph_dir" ]]; then
    flamegraph_output="${OUTPUT_DIR}/flamegraph.svg"
    echo "Writing ${flamegraph_output}..." >&2
    "${resolved_flamegraph_dir}/stackcollapse-perf.pl" "$PERF_SCRIPT" \
      | "${resolved_flamegraph_dir}/flamegraph.pl" > "$flamegraph_output"
  else
    cat >&2 <<EOF
warning: FlameGraph scripts were not found; perf.data and perf.script are available.
Re-run with --input "$PERF_DATA" --install-flamegraph, or pass --flamegraph-dir.
EOF
  fi
fi

FindHotspot() {
  local candidate
  if [[ -n "$HOTSPOT_BIN" ]]; then
    [[ -x "$HOTSPOT_BIN" ]] || Die "Hotspot is not executable: $HOTSPOT_BIN"
    printf '%s\n' "$HOTSPOT_BIN"
    return
  fi
  if command -v hotspot >/dev/null 2>&1; then
    command -v hotspot
    return
  fi
  for candidate in "${HOME}"/Downloads/hotspot-*.AppImage "${HOME}"/Applications/hotspot-*.AppImage; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return
    fi
  done
  return 1
}

if [[ $OPEN_HOTSPOT -eq 1 ]]; then
  resolved_hotspot=$(FindHotspot) || Die "Hotspot not found; use --hotspot-bin <path>"
  echo "Opening ${PERF_DATA} with ${resolved_hotspot}..." >&2
  "$resolved_hotspot" "$PERF_DATA" >/dev/null 2>&1 &
fi

echo "Profile artifacts: ${OUTPUT_DIR}" >&2
exit "$record_status"