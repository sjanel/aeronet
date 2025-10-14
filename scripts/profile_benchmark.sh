#!/usr/bin/env bash
# Simple helper to profile a benchmark binary with perf and produce output
# consumable by kcachegrind (callgrind format) or FlameGraph (SVG).
#
# Usage:
#   ./scripts/profile_benchmark.sh --build -- ./build/benchmarks/throughput --arg val
# Options:
#   --build    : run a CMake configure/build step (RelWithDebInfo, frame pointers)
#   --freq N   : sampling frequency for perf record (default 200)
#
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

FREQ=200
DO_BUILD=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build)
      DO_BUILD=1; shift;;
    --freq)
      FREQ="$2"; shift 2;;
    --)
      shift; break;;
    -h|--help)
      sed -n '1,200p' "$0"; exit 0;;
    *)
      break;;
  esac
done

BINARY=""
ARGS=()
if [[ $# -ge 1 ]]; then
  BINARY="$1"; shift
  ARGS=("$@")
fi

if [[ $DO_BUILD -eq 1 && -z "$BINARY" ]]; then
  echo "Configuring and building with frame-pointers and debuginfo..."
  cmake -S . -B build -G Ninja \
    -DAERONET_BUILD_TESTS=0 \
    -DAERONET_BUILD_EXAMPLES=0 \
    -DAERONET_BUILD_BENCHMARKS=1 \
    -DAERONET_BENCH_ENABLE_DROGON=0 \
    -DAERONET_BENCH_ENABLE_OATPP=0 \
    -DAERONET_BENCH_ENABLE_HTTPLIB=0 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS_RELEASE="-g -fno-omit-frame-pointer -O2" \
    -DCMAKE_C_FLAGS_RELEASE="-g -fno-omit-frame-pointer -O2"
  cmake --build build -j
  echo "Build complete. Re-run the script with the benchmark binary to profile it, e.g."
  echo "  $0 -- ./build/benchmarks/aeronet-bench-throughput --duration 10"
  exit 0
fi

if [[ -z "$BINARY" ]]; then
  echo "Usage: $0 [--build] [--freq N] -- <benchmark-binary> [args...]"
  exit 2
fi

# Locate a usable perf binary. /usr/bin/perf is a wrapper that expects a
# kernel-specific binary under /usr/lib/linux-tools/$(uname -r)/perf. On some
# distros that wrapper prints a helpful hint and exits with non-zero; in that
# case search for an existing perf binary from other linux-tools packages and
# use it.
PERF_BIN=""
if command -v perf >/dev/null 2>&1; then
  # test if the wrapper resolves to a usable binary
  if perf --version >/dev/null 2>&1; then
    PERF_BIN="$(command -v perf)"
  else
    PERF_BIN=""
  fi
fi

if [[ -z "$PERF_BIN" ]]; then
  # search known locations for a perf binary installed by linux-tools packages
  for p in /usr/lib/linux-tools-*/perf /usr/lib/linux-tools/*/perf /usr/lib/linux-hwe-*/perf; do
    if [[ -x "$p" ]]; then
      PERF_BIN="$p"
      break
    fi
  done
fi

if [[ -z "$PERF_BIN" ]]; then
  echo "Error: 'perf' not found or wrapper points to missing kernel-specific binary." >&2
  echo "On Ubuntu you can try: sudo apt install linux-tools-$(uname -r) linux-cloud-tools-$(uname -r)" >&2
  echo "Or create a symlink from an existing perf binary into /usr/lib/linux-tools/$(uname -r)/perf" >&2
  exit 1
else
  echo "Using perf: $PERF_BIN"
fi

# Timestamped perf data file
TS=$(date +%Y%m%d-%H%M%S)
PERF_DATA="perf.data.${TS}"

echo "Recording perf data to ${PERF_DATA} (freq=${FREQ})..."
sudo "$PERF_BIN" record -F "${FREQ}" -g -o "${PERF_DATA}" -- "${BINARY}" "${ARGS[@]}"

# perf record runs as root, so the output file is owned by root. Change ownership
# to the current user so subsequent perf script/conversion commands can read it.
if [[ -f "${PERF_DATA}" ]]; then
  sudo chown "$(id -u):$(id -g)" "${PERF_DATA}"
  # also chown any related files perf may have written with the same prefix
  prefix="${PERF_DATA%.*}"
  # iterate only if the glob matches
  if compgen -G "${prefix}*" > /dev/null; then
    for f in ${prefix}*; do
      if [[ -e "$f" ]]; then
        sudo chown "$(id -u):$(id -g)" "$f" || true
      fi
    done
  fi
fi

echo "Generating perf script..."
"$PERF_BIN" script -i "${PERF_DATA}" > "perf_script.${TS}.txt"
CALLGRIND_OUT="callgrind.${TS}.out"

# Optional filtering: keep only samples that match PERF_INCLUDE and do not match PERF_EXCLUDE
FILTERED_PERF_SCRIPT="perf_script.${TS}.filtered.txt"
if [[ -n "${PERF_INCLUDE:-}" || -n "${PERF_EXCLUDE:-}" ]]; then
  echo "Filtering perf script with PERF_INCLUDE='${PERF_INCLUDE:-}' PERF_EXCLUDE='${PERF_EXCLUDE:-}'..."
  python3 "${SCRIPT_DIR}/_ext/filter_perf_script.py" "perf_script.${TS}.txt" "${FILTERED_PERF_SCRIPT}" --include "${PERF_INCLUDE:-}" --exclude "${PERF_EXCLUDE:-}"
  PERF_SCRIPT_TO_USE="${FILTERED_PERF_SCRIPT}"
else
  PERF_SCRIPT_TO_USE="perf_script.${TS}.txt"
fi
if command -v perf2calltree >/dev/null 2>&1; then
  echo "Converting to callgrind via perf2calltree..."
  # Prefer converting from the (optionally filtered) perf script
  perf2calltree < "${PERF_SCRIPT_TO_USE}" > "${CALLGRIND_OUT}"
  echo "Callgrind output: ${CALLGRIND_OUT}"
  echo "Open with: kcachegrind ${CALLGRIND_OUT}"
  exit 0
fi

if [[ -x "/usr/share/kcachegrind/perf2calltree" ]]; then
  echo "Using /usr/share/kcachegrind/perf2calltree to convert..."
  "$PERF_BIN" script -i "${PERF_DATA}" | "/usr/share/kcachegrind/perf2calltree" > "${CALLGRIND_OUT}"
  echo "Callgrind output: ${CALLGRIND_OUT}"
  echo "Open with: kcachegrind ${CALLGRIND_OUT}"
  exit 0
fi

echo "perf2calltree not found. Trying gprof2calltree (py) if installed..."
if command -v gprof2calltree >/dev/null 2>&1; then
  echo "Converting via gprof2calltree (expects gprof-like input). Attempting perf script -> gprof2calltree..."
  # Use the filtered perf script if present
  cp "${PERF_SCRIPT_TO_USE}" perf_for_gprof.${TS}.txt
  gprof2calltree -i perf_for_gprof.${TS}.txt -o "${CALLGRIND_OUT}"
  echo "Callgrind output: ${CALLGRIND_OUT}"
  echo "Open with: kcachegrind ${CALLGRIND_OUT}"
  exit 0
fi

echo "Falling back to FlameGraph generation (if you have Brendan Gregg's FlameGraph scripts)..."
if [[ -d "${SCRIPT_DIR}/FlameGraph" ]]; then
  echo "Using local FlameGraph/stackcollapse-perf.pl and flamegraph.pl"
  # use filtered script if available for flamegraph
  if [[ -f "${PERF_SCRIPT_TO_USE}" ]]; then
    "${SCRIPT_DIR}/FlameGraph/stackcollapse-perf.pl" "${PERF_SCRIPT_TO_USE}" | "${SCRIPT_DIR}/FlameGraph/flamegraph.pl" > "flamegraph.${TS}.svg"
  else
    "$PERF_BIN" script -i "${PERF_DATA}" | "${SCRIPT_DIR}/FlameGraph/stackcollapse-perf.pl" | "${SCRIPT_DIR}/FlameGraph/flamegraph.pl" > "flamegraph.${TS}.svg"
  fi
  echo "FlameGraph: flamegraph.${TS}.svg"
  # Also try our local minimal converter as a fallback to produce a callgrind file
  if [[ -x "${SCRIPT_DIR}/_ext/perf_script_to_callgrind.py" || -f "${SCRIPT_DIR}/_ext/perf_script_to_callgrind.py" ]]; then
    echo "Also running local perf_script_to_callgrind.py fallback to generate callgrind..."
    python3 "${SCRIPT_DIR}/_ext/perf_script_to_callgrind.py" "perf_script.${TS}.txt" "${CALLGRIND_OUT}" || true
    if [[ -f "${CALLGRIND_OUT}" ]]; then
      echo "Callgrind output (fallback): ${CALLGRIND_OUT}"
      echo "Open with: kcachegrind ${CALLGRIND_OUT}"
    fi
  fi
  exit 0
fi

echo "No conversion tool found. Helpful next steps:" 
echo " - Install kcachegrind and perf2calltree (Debian/Ubuntu: sudo apt install kcachegrind)" 
echo " - If perf2calltree not packaged: get the script from perf-tools repo or check /usr/share/kcachegrind/perf2calltree" 
echo " - Alternatively clone FlameGraph and run:"
echo "     git clone https://github.com/brendangregg/FlameGraph.git ${SCRIPT_DIR}/FlameGraph"
echo "     perf script -i ${PERF_DATA} | ${SCRIPT_DIR}/FlameGraph/stackcollapse-perf.pl | ${SCRIPT_DIR}/FlameGraph/flamegraph.pl > flamegraph.${TS}.svg"

echo "Perf raw script is at perf_script.${TS}.txt if you want to inspect frames." 
