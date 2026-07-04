#!/usr/bin/env python3
"""Orchestrate the scripted *client* benchmarks.

Starts a single over-provisioned aeronet-bench-server (many threads, so it is never the bottleneck), then
runs each compiled client driver (aeronet HttpClient vs libcurl / drogon / beast) across a set of scenarios.
Each driver emits a single-line JSON result which we collect, then print a per-scenario comparison table and
write JSON (and optionally HTML) artifacts.

Usage:
    ./run_client_benchmarks.py                          # all clients, all scenarios, defaults
    ./run_client_benchmarks.py --client aeronet,curl    # subset of clients
    ./run_client_benchmarks.py --scenario small-get     # subset of scenarios
    ./run_client_benchmarks.py --threads 8 --duration 15s --html
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# aeronet driver is always present; the others depend on what was built (libcurl / drogon / beast).
CLIENT_ORDER = ["aeronet", "curl", "drogon", "beast"]
SCENARIO_ORDER = ["small-get", "headers", "large-get", "post", "json", "compress", "no-reuse"]

# Protocols mirror run_benchmarks.py (the scripted-server runner): http1 (HTTP/1.1), h2c (cleartext HTTP/2,
# prior knowledge) and h2-tls (HTTP/2 over TLS, ALPN "h2"). For the HTTP/2 protocols we only measure the
# clients that actually speak HTTP/2 (aeronet's native engine and libcurl via nghttp2); Drogon's sync client
# and Boost.Beast are HTTP/1.1-only and would otherwise silently downgrade, so they are skipped.
PROTOCOL_CHOICES = ["http1", "h2c", "h2-tls"]
H2_CAPABLE_CLIENTS = {"aeronet", "curl"}

# Per-scenario request/response body size (bytes) passed via --body-size. Other scenarios ignore it.
SCENARIO_BODY_SIZE: Dict[str, int] = {
    "large-get": 1 << 20,  # 1 MiB response body
    "post": 4096,          # 4 KiB request body
}

DEFAULT_PORT = 8090


class BenchError(RuntimeError):
    pass


def find_build_dir(script_dir: Path) -> Path:
    """Locate the build tree that contains the compiled client drivers."""
    candidates = [
        script_dir / "../../build-release",
        script_dir / "../../build",
        script_dir / "../../build-pages",
        script_dir / "../build-release",
        script_dir / "../build",
    ]
    for cand in candidates:
        if (cand / "benchmarks/scripted-clients/aeronet-bench-client").is_file():
            return cand.resolve()
    raise BenchError(
        "Could not find a build dir with aeronet-bench-client. Build first, e.g.:\n"
        "  cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && ninja -C build-release"
    )


def wait_for_server(port: int, timeout_s: float = 15.0) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.05)
    raise BenchError(f"Server did not become ready on port {port} within {timeout_s}s")


def find_certs(script_dir: Path) -> Tuple[Path, Path]:
    """Locate (and, if missing, generate) the self-signed TLS cert/key the h2-tls server needs. Reuses the
    scripted-server certs so both suites share one CA."""
    certs_dir = script_dir.parent / "scripted-servers" / "certs"
    cert, key = certs_dir / "server.crt", certs_dir / "server.key"
    if cert.is_file() and key.is_file():
        return cert, key
    setup = script_dir.parent / "scripted-servers" / "setup_bench_resources.py"
    if setup.is_file():
        print(f"  (generating TLS certs via {setup.name})")
        subprocess.run([sys.executable, str(setup), "--output", str(certs_dir.parent)], check=False)
    if cert.is_file() and key.is_file():
        return cert, key
    raise BenchError(
        f"h2-tls needs a TLS cert/key at {certs_dir} but none were found and auto-generation failed. "
        "Generate them first, e.g.:\n"
        f"  {sys.executable} {setup} --output {certs_dir.parent}"
    )


def start_server(server_bin: Path, port: int, threads: int, protocol: str,
                 certs: Optional[Tuple[Path, Path]] = None) -> subprocess.Popen:
    cmd = [str(server_bin), "--port", str(port), "--threads", str(threads)]
    if protocol in ("h2c", "h2-tls"):
        cmd.append("--h2")  # h2c is on by default, but be explicit (and it drives ALPN under TLS)
    if protocol == "h2-tls":
        if certs is None:
            raise BenchError("h2-tls requires a TLS cert/key (internal error: certs not provided)")
        cert, key = certs
        cmd += ["--tls", "--cert", str(cert), "--key", str(key)]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for_server(port)
    except BenchError:
        proc.terminate()
        raise
    return proc


def stop_server(proc: subprocess.Popen) -> None:
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def run_driver(
    binary: Path,
    base_url: str,
    scenario: str,
    threads: int,
    duration: str,
    warmup: str,
    protocol: str,
) -> Optional[dict]:
    cmd = [
        str(binary),
        "--url", base_url,
        "--scenario", scenario,
        "--protocol", protocol,
        "--threads", str(threads),
        "--duration", duration,
        "--warmup", warmup,
        "--json",
    ]
    body_size = SCENARIO_BODY_SIZE.get(scenario)
    if body_size is not None:
        cmd += ["--body-size", str(body_size)]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        print(f"  ! {binary.name} timed out on {scenario}", file=sys.stderr)
        return None
    # The JSON result is the last non-empty stdout line (drivers may print warnings to stderr).
    for line in reversed(out.stdout.strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                break
    print(f"  ! {binary.name} produced no result on {scenario} (exit {out.returncode})", file=sys.stderr)
    if out.stderr.strip():
        print("    " + out.stderr.strip().replace("\n", "\n    "), file=sys.stderr)
    return None


def fmt_rps(rps: float) -> str:
    if rps >= 1e6:
        return f"{rps / 1e6:.2f}M"
    if rps >= 1e3:
        return f"{rps / 1e3:.1f}k"
    return f"{rps:.0f}"


def _fmt_latency_us(us: float) -> str:
    """Format a microsecond latency the way wrk renders it (us / ms / s), so the comparison
    box and its best-of detection match the scripted-server tables."""
    if us >= 1_000_000:
        return f"{us / 1_000_000:.2f}s"
    if us >= 1000:
        return f"{us / 1000:.2f}ms"
    return f"{us:.2f}us"


def _table_printer():
    """Import the scripted-server TablePrinter so client and server suites render the same boxes."""
    server_dir = Path(__file__).resolve().parent.parent / "scripted-servers"
    if str(server_dir) not in sys.path:
        sys.path.insert(0, str(server_dir))
    try:
        from bench_utils import TablePrinter  # noqa: local import
    except ImportError as exc:  # bench_utils lives next to run_benchmarks.py
        raise BenchError(f"could not import TablePrinter from {server_dir}: {exc}") from exc
    return TablePrinter


def print_summary(results: List[dict], clients: List[str], scenarios: List[str]) -> None:
    """Print per-metric comparison boxes in the exact style of the scripted-server runner
    (run_benchmarks.py), reusing its TablePrinter. Clients map onto the table's column axis."""
    TablePrinter = _table_printer()
    rps: Dict[Tuple[str, str], str] = {}
    p50: Dict[Tuple[str, str], str] = {}
    p99: Dict[Tuple[str, str], str] = {}
    transfer: Dict[Tuple[str, str], str] = {}
    memory: Dict[Tuple[str, str], str] = {}
    for r in results:
        key = (r["client"], r["scenario"])
        rps[key] = f"{r['rps']:.2f}"
        p50[key] = _fmt_latency_us(r["p50_us"])
        p99[key] = _fmt_latency_us(r["p99_us"])
        transfer[key] = f"{r['bytes_per_s'] / (1024 * 1024):.2f}MB"
        memory[key] = f"{r['rss_kb'] / 1024:.1f}MB"
    TablePrinter(
        clients,
        scenarios,
        metrics=[
            ("BENCHMARK RESULTS COMPARISON", "(Successful responses/sec - higher is better)", rps, True),
            ("LATENCY COMPARISON (p50)", "(Median latency - lower is better)", p50, False),
            ("LATENCY COMPARISON (p99)", "(Tail latency - lower is better)", p99, False),
            ("TRANSFER RATE COMPARISON", "(Data throughput - higher is better)", transfer, True),
            ("MEMORY USAGE", "(Resident set size - lower is better)", memory, False),
        ],
    ).print_all()


def to_pages_summary(results: List[dict], meta: dict, scenarios: List[str], clients: List[str]) -> dict:
    """Convert collected results into the exact schema render_benchmarks_html.py (the scripted-server
    renderer) consumes, so the HTML dashboard and badge are produced by the very same code. Clients map
    onto the schema's `servers` axis; metric values are unit-suffixed strings as wrk emits them."""
    out_results: Dict[str, dict] = {}
    for scenario in scenarios:
        rows = [r for r in results if r["scenario"] == scenario]
        if not rows:
            continue
        rps: Dict[str, str] = {}
        latency: Dict[str, str] = {}
        transfer: Dict[str, str] = {}
        memory: Dict[str, dict] = {}
        for r in rows:
            client = r["client"]
            rps[client] = f"{r['rps']:.2f}"
            latency[client] = f"{r['avg_us'] / 1000.0:.3f}ms"
            transfer[client] = f"{r['bytes'] / (1024 * 1024):.2f}MB"  # total payload moved over the run
            rss_mb = round(r["rss_kb"] / 1024.0, 3)
            memory[client] = {"rss_mb": rss_mb, "peak_mb": rss_mb, "swap_mb": 0.0,
                              "threads": meta["threads"], "vmhwm_mb": rss_mb, "vmsize_mb": rss_mb}
        winner = max(rps, key=lambda c: float(rps[c])) if rps else None
        out_results[scenario] = {
            "rps": rps, "latency": latency, "transfer": transfer, "memory": memory,
            "timeouts": {c: 0 for c in rps}, "winners": {"rps": winner} if winner else {},
        }
    return {
        "protocol": meta["protocol"],
        "tool": "scripted-clients",
        "threads": meta["threads"],
        "connections": meta["threads"],  # one keep-alive connection per worker thread
        "duration": meta["duration"],
        "warmup": meta["warmup"],
        "servers": [c for c in clients if any(r["client"] == c for r in results)],
        "scenarios": [s for s in scenarios if any(r["scenario"] == s for r in results)],
        "results": out_results,
    }


def _badge_value(value: float) -> str:
    if value >= 1_000_000:
        return f"{value / 1_000_000:.1f}M"
    if value >= 1_000:
        return f"{value / 1_000:.0f}k"
    return f"{value:.0f}"


def _badge_color(value: float) -> str:
    if value >= 200_000:
        return "brightgreen"
    if value >= 100_000:
        return "green"
    if value >= 50_000:
        return "yellowgreen"
    if value >= 10_000:
        return "yellow"
    return "lightgrey"


def write_artifacts(summary: dict, out_dir: Path) -> Path:
    """Write the render-compatible `client_benchmark_latest[_<proto>].json`, a timestamped copy, and the
    shields.io endpoint badge (aeronet peak rps, mirroring the scripted-server badge). The default http1
    protocol keeps the historical unsuffixed filenames (the gh-pages workflow reads them); h2c / h2-tls get
    a protocol suffix so running several protocols in a row does not clobber one another's artifacts."""
    protocol = summary["protocol"]
    suffix = "" if protocol == "http1" else f"_{protocol}"
    out_dir.mkdir(parents=True, exist_ok=True)
    latest = out_dir / f"client_benchmark_latest{suffix}.json"
    latest.write_text(json.dumps(summary, indent=2))
    stamp = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    (out_dir / f"client_benchmark{suffix}_{stamp}.json").write_text(json.dumps(summary, indent=2))

    best = 0.0
    for data in summary["results"].values():
        value = data.get("rps", {}).get("aeronet")
        if value is not None:
            try:
                best = max(best, float(value))
            except ValueError:
                pass
    if best > 0:
        label = "client peak rps" if protocol == "http1" else f"client peak rps ({protocol})"
        badge = {
            "schemaVersion": 1,
            "label": label,
            "message": f"{_badge_value(best)} req/s",
            "color": _badge_color(best),
            "labelColor": "#0f172a",
            "namedLogo": "speedtest",
            "cacheSeconds": 3600,
        }
        (out_dir / f"client_benchmark_badge{suffix}.json").write_text(json.dumps(badge, indent=2))
    return latest


def render_html(latest_json: Path, script_dir: Path, out_dir: Path, protocol: str) -> Optional[Path]:
    """Render the HTML dashboard by reusing the scripted-server renderer (render_benchmarks_html.py)."""
    renderer = script_dir.parent / "scripted-servers" / "render_benchmarks_html.py"
    if not renderer.is_file():
        print(f"  (skipping HTML: renderer not found at {renderer})", file=sys.stderr)
        return None
    suffix = "" if protocol == "http1" else f"_{protocol}"
    html_path = out_dir / f"client_benchmark{suffix}.html"
    subprocess.run([sys.executable, str(renderer), "--input", str(latest_json), "--output", str(html_path)],
                   check=True)
    return html_path


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--client", default=",".join(CLIENT_ORDER), help="comma-separated client subset")
    parser.add_argument("--scenario", default=",".join(SCENARIO_ORDER), help="comma-separated scenario subset")
    parser.add_argument(
        "--protocol", default="http1", choices=PROTOCOL_CHOICES,
        help="protocol to benchmark: http1 (HTTP/1.1), h2c (cleartext HTTP/2), h2-tls (HTTP/2 over TLS). "
             "h2c/h2-tls only measure HTTP/2-capable clients (aeronet, curl).",
    )
    parser.add_argument("--threads", type=int, default=4, help="client worker threads / connections")
    parser.add_argument(
        "--server-threads", type=int, default=os.cpu_count() or 8,
        help="aeronet-bench-server threads (kept high so the server is not the bottleneck)",
    )
    parser.add_argument("--duration", default="30s", help="measured window per run")
    parser.add_argument("--warmup", default="5s", help="warmup window per run")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--build-dir", default=None, help="override build dir auto-detection")
    parser.add_argument("--output", default=str(script_dir / "results"), help="artifact output directory")
    parser.add_argument("--html", action="store_true", help="also write an HTML report")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve() if args.build_dir else find_build_dir(script_dir)
    server_bin = build_dir / "benchmarks/scripted-servers/aeronet-bench-server"
    if not server_bin.is_file():
        raise BenchError(f"aeronet-bench-server not found at {server_bin}")
    client_dir = build_dir / "benchmarks/scripted-clients"

    protocol = args.protocol
    is_h2 = protocol in ("h2c", "h2-tls")

    requested_clients = [c.strip() for c in args.client.split(",") if c.strip()]
    clients = [c for c in CLIENT_ORDER if c in requested_clients]
    available = []
    for name in clients:
        if is_h2 and name not in H2_CAPABLE_CLIENTS:
            print(f"  (skipping {name}: HTTP/1.1-only, not measured for {protocol})", file=sys.stderr)
            continue
        binary = client_dir / f"{name}-bench-client"
        if binary.is_file():
            available.append((name, binary))
        else:
            print(f"  (skipping {name}: driver not built)", file=sys.stderr)
    if not available:
        raise BenchError("No client drivers available to run")

    requested_scenarios = [s.strip() for s in args.scenario.split(",") if s.strip()]
    scenarios = [s for s in SCENARIO_ORDER if s in requested_scenarios]

    certs = find_certs(script_dir) if protocol == "h2-tls" else None
    scheme = "https" if protocol == "h2-tls" else "http"
    base_url = f"{scheme}://127.0.0.1:{args.port}"
    meta = {
        "date": _dt.datetime.now().isoformat(timespec="seconds"),
        "protocol": protocol,
        "threads": args.threads,
        "server_threads": args.server_threads,
        "duration": args.duration,
        "warmup": args.warmup,
        "clients": [n for n, _ in available],
        "scenarios": scenarios,
    }

    print(f"Server : {server_bin.name} --threads {args.server_threads} (port {args.port}) [{protocol}]")
    print(f"Clients: {', '.join(n for n, _ in available)}")
    print(f"Threads: {args.threads}  duration: {args.duration}  warmup: {args.warmup}")

    server = start_server(server_bin, args.port, args.server_threads, protocol, certs)
    results: List[dict] = []
    try:
        for scenario in scenarios:
            print(f"\n--- {scenario} ---")
            for name, binary in available:
                res = run_driver(binary, base_url, scenario, args.threads, args.duration, args.warmup, protocol)
                if res is None:
                    continue
                results.append(res)
                print(
                    f"  {name:<10} {fmt_rps(res['rps']):>9} rps  "
                    f"p50={res['p50_us']:.1f}us p99={res['p99_us']:.1f}us  "
                    f"{res['bytes_per_s'] / (1024 * 1024):.1f} MB/s  RSS={res['rss_kb'] / 1024:.1f}MB"
                )
    finally:
        stop_server(server)

    if not results:
        raise BenchError("No results collected")

    client_names = [n for n, _ in available]
    print_summary(results, client_names, scenarios)
    out_dir = Path(args.output)
    summary = to_pages_summary(results, meta, scenarios, client_names)
    latest = write_artifacts(summary, out_dir)
    print(f"\nJSON  -> {latest}")
    if args.html:
        html_path = render_html(latest, script_dir, out_dir, protocol)
        if html_path is not None:
            print(f"HTML  -> {html_path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BenchError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
