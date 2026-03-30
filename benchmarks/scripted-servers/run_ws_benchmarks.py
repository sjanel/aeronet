#!/usr/bin/env python3
"""WebSocket benchmark orchestration script.

Launches C++ benchmark servers, runs k6 and optionally websocket-bench against
each, and produces a unified results summary (text + JSON).

Usage:
    ./run_ws_benchmarks.py [options]
    ./run_ws_benchmarks.py --server aeronet,drogon --scenario echo-small,churn
    ./run_ws_benchmarks.py --smoke   # Quick validation run (5s, 10 VUs)
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

# ----------------------------- Constants ----------------------------------- #

K6_SCENARIOS: Dict[str, str] = {
    "echo-small": "k6/ws_echo_small.js",
    "echo-medium": "k6/ws_echo_medium.js",
    "mix": "k6/ws_mix_text_binary.js",
    "ping-pong": "k6/ws_ping_pong.js",
    "churn": "k6/ws_churn.js",
    "compression": "k6/ws_compression.js",
}

# Scenarios that use the /ws-compressed endpoint (permessage-deflate).
# All other scenarios use /ws-uncompressed.
COMPRESSED_SCENARIOS = {"compression"}

# Servers that support the /ws-compressed endpoint.
# Drogon does not implement permessage-deflate.
COMPRESSION_CAPABLE_SERVERS = {"aeronet", "uwebsockets"}

SERVER_PORTS: Dict[str, int] = {
    "aeronet": 8080,
    "drogon": 8081,
    "uwebsockets": 8088,
}

SERVER_ORDER = ["aeronet", "uwebsockets", "drogon"]


@dataclass
class RunResult:
    scenario: str
    server: str
    tool: str  # "k6" or "websocket-bench"
    metrics: Dict[str, Any] = field(default_factory=dict)
    raw_output: str = ""
    success: bool = True


class WsBenchmarkError(RuntimeError):
    pass


# ----------------------------- Runner -------------------------------------- #


class WsBenchmarkRunner:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.script_dir = Path(__file__).resolve().parent
        self.build_dir = self._find_build_dir()

        self.vus = args.vus
        self.duration = args.duration
        self.warmup = args.warmup
        self.session_duration_ms = args.session_duration_ms
        self.k6_instances = args.k6_instances
        self.pipeline_depth = args.pipeline_depth

        self.output_dir = Path(args.output).resolve()
        self.output_dir.mkdir(parents=True, exist_ok=True)
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        self.result_file = self.output_dir / f"ws_benchmark_{timestamp}.txt"
        self.json_file = self.output_dir / f"ws_benchmark_{timestamp}.json"
        self.html_file = self.output_dir / f"ws_benchmark_{timestamp}.html"

        self.servers_to_test = self._resolve_servers(args.server)
        self.scenarios_to_test = self._resolve_scenarios(args.scenario)
        self.enable_ws_bench = args.websocket_bench

        self.server_processes: Dict[str, subprocess.Popen] = {}
        self.results: List[RunResult] = []

        # Track whether any aeronet scenario reported errors (fails CI)
        self._aeronet_errors_found: bool = False

    # ----------------------- Setup helpers --------------------------------- #

    def _find_build_dir(self) -> Path:
        build_dir_env = os.environ.get("AERONET_BUILD_DIR")
        if build_dir_env:
            env_path = Path(build_dir_env).resolve()
            if env_path.is_dir():
                return env_path

        # When invoked from a symlink inside the build tree, CWD is the
        # build dir itself — check it first.
        cwd = Path.cwd().resolve()

        candidates = [
            cwd,
            self.script_dir / "../../build-pages/benchmarks/scripted-servers",
            self.script_dir / "../../build-release/benchmarks/scripted-servers",
            self.script_dir / "../../build/benchmarks/scripted-servers",
            self.script_dir / "../build-release/benchmarks/scripted-servers",
            self.script_dir / "../build/benchmarks/scripted-servers",
        ]
        for cand in candidates:
            resolved = cand.resolve()
            if resolved.is_dir() and (resolved / "aeronet-bench-server").is_file():
                return resolved
        return self.script_dir

    def _resolve_servers(self, server_arg: str) -> List[str]:
        if server_arg == "all":
            return [s for s in SERVER_ORDER if self._server_available(s)]
        names = [s.strip() for s in server_arg.split(",") if s.strip()]
        for name in names:
            if name not in SERVER_PORTS:
                raise WsBenchmarkError(f"Unknown server: {name}")
        return names

    def _resolve_scenarios(self, scenario_arg: str) -> List[str]:
        if scenario_arg == "all":
            return list(K6_SCENARIOS.keys())
        names = [s.strip() for s in scenario_arg.split(",") if s.strip()]
        for name in names:
            if name not in K6_SCENARIOS:
                raise WsBenchmarkError(
                    f"Unknown scenario: {name}. Available: {', '.join(K6_SCENARIOS)}"
                )
        return names

    def _server_available(self, name: str) -> bool:
        try:
            self._server_binary(name)
            return True
        except WsBenchmarkError:
            return False

    def _server_binary(self, name: str) -> Path:
        binary = self.build_dir / f"{name}-bench-server"
        if not binary.is_file():
            raise WsBenchmarkError(f"Binary not found: {binary}")
        return binary

    @staticmethod
    def _wait_for_port(port: int, timeout: float = 10.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                    return True
            except OSError:
                time.sleep(0.1)
        return False

    # ----------------------- Server lifecycle ------------------------------ #

    def _start_server(self, name: str) -> bool:
        if name in self.server_processes:
            return True
        port = SERVER_PORTS[name]
        binary = self._server_binary(name)
        cmd = [str(binary), "--port", str(port), "--threads", str(int((self.args.threads + 3) / 4))]
        log_path = self.output_dir / f"{name}_server.log"
        log_fp = open(log_path, "w")
        proc = subprocess.Popen(
            cmd, stdout=log_fp, stderr=subprocess.STDOUT, preexec_fn=os.setsid
        )
        self.server_processes[name] = proc
        if not self._wait_for_port(port):
            print(f"  ERROR: {name} did not start on port {port}")
            self._stop_server(name)
            return False
        print(f"  {name} started (pid={proc.pid}, port={port})")
        return True

    def _stop_server(self, name: str) -> None:
        proc = self.server_processes.pop(name, None)
        if proc is None:
            return
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            proc.wait(timeout=5)
        except Exception:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                proc.wait(timeout=3)
            except Exception:
                pass

    def _stop_all(self) -> None:
        for name in list(self.server_processes):
            self._stop_server(name)

    # ----------------------- k6 execution --------------------------------- #

    def _run_k6(self, server: str, scenario: str) -> RunResult:
        port = SERVER_PORTS[server]
        ws_path = "/ws-compressed" if scenario in COMPRESSED_SCENARIOS else "/ws-uncompressed"
        ws_url = f"ws://127.0.0.1:{port}{ws_path}"
        script = self.script_dir / K6_SCENARIOS[scenario]
        if not script.is_file():
            script = self.build_dir / K6_SCENARIOS[scenario]
        if not script.is_file():
            return RunResult(
                scenario=scenario,
                server=server,
                tool="k6",
                success=False,
                raw_output=f"Script not found: {K6_SCENARIOS[scenario]}",
            )

        n_instances = max(1, self.k6_instances)
        vus_per = max(1, self.vus // n_instances)

        # Build common env
        base_env = os.environ.copy()
        base_env.update({
            "WS_URL": ws_url,
            "DURATION": self.duration,
            "SESSION_DURATION_MS": str(self.session_duration_ms),
        })
        if self.pipeline_depth > 0:
            base_env["PIPELINE_DEPTH"] = str(self.pipeline_depth)

        # Limit Go threads per instance so k6 doesn't steal server CPU
        cpu_count = os.cpu_count() or 1
        client_cores = max(1, cpu_count - (self.args.threads if hasattr(self.args, "threads") else cpu_count // 2))
        gomaxprocs = max(2, client_cores // n_instances)
        base_env["GOMAXPROCS"] = str(gomaxprocs)

        # Launch parallel k6 instances
        procs: List[subprocess.Popen] = []
        json_files: List[Path] = []
        for idx in range(n_instances):
            json_out = self.output_dir / f"k6_{server}_{scenario}_{idx}.json"
            json_files.append(json_out)
            env = base_env.copy()
            env["VUS"] = str(vus_per)
            cmd = [
                "k6", "run",
                "--summary-export", str(json_out),
                "--quiet",
                str(script),
            ]
            try:
                proc = subprocess.Popen(
                    cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
                )
                procs.append(proc)
            except FileNotFoundError:
                # Kill already started instances
                for pp in procs:
                    pp.kill()
                    pp.wait()
                return RunResult(
                    scenario=scenario,
                    server=server,
                    tool="k6",
                    success=False,
                    raw_output="k6 binary not found. Install: https://k6.io/docs/get-started/installation/",
                )

        # Wait for all instances to finish
        outputs: List[str] = []
        all_success = True
        for proc in procs:
            try:
                stdout, stderr = proc.communicate(timeout=300)
                outputs.append(stdout.decode(errors="replace") + stderr.decode(errors="replace"))
                if proc.returncode != 0:
                    all_success = False
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                outputs.append("k6 instance timed out after 300s")
                all_success = False

        # Aggregate results from all instances
        metrics = self._aggregate_k6_jsons(json_files)

        # Clean up per-instance files, keep a merged summary
        merged_json = self.output_dir / f"k6_{server}_{scenario}.json"
        if metrics:
            with open(merged_json, "w") as fp:
                json.dump({"metrics": metrics, "_k6_instances": n_instances}, fp, indent=2)
        for jf in json_files:
            try:
                jf.unlink(missing_ok=True)
            except OSError:
                pass

        return RunResult(
            scenario=scenario,
            server=server,
            tool="k6",
            metrics=metrics,
            raw_output="\n".join(outputs),
            success=all_success,
        )

    @staticmethod
    def _aggregate_k6_jsons(json_paths: List[Path]) -> Dict[str, Any]:
        """Merge results from parallel k6 instances."""
        all_metrics = [
            WsBenchmarkRunner._parse_k6_json(p)
            for p in json_paths if p.is_file()
        ]
        all_metrics = [m for m in all_metrics if m]
        if not all_metrics:
            return {}
        if len(all_metrics) == 1:
            return all_metrics[0]

        merged: Dict[str, Any] = {}

        # Counter keys: sum count and rate across instances
        counter_keys = (
            "ws_messages_sent", "ws_messages_received",
            "ws_msgs_sent", "ws_msgs_received",
            "ws_pings_sent", "ws_pongs_received",
            "ws_connections_opened", "ws_connections_closed",
            "ws_sessions", "iterations", "data_sent", "data_received",
        )
        for key in counter_keys:
            values = [m[key] for m in all_metrics if key in m]
            if values:
                merged[key] = {
                    "count": sum(v.get("count", 0) for v in values),
                    "rate": sum(v.get("rate", 0) for v in values),
                }

        # Trend keys: avg/med averaged, percentiles/max take worst case
        trend_keys = (
            "ws_echo_rtt_ms", "ws_ping_rtt_ms", "ws_connection_lifetime_ms",
            "ws_connecting", "ws_session_duration", "iteration_duration",
        )
        for key in trend_keys:
            values = [m[key] for m in all_metrics if key in m]
            if values:
                nn = len(values)
                merged[key] = {
                    "avg": sum(v.get("avg", 0) for v in values) / nn,
                    "med": sum(v.get("med", 0) for v in values) / nn,
                    "p90": max(v.get("p90", 0) for v in values),
                    "p95": max(v.get("p95", 0) for v in values),
                    "p99": max(v.get("p99", 0) for v in values),
                    "min": min(v.get("min", float("inf")) for v in values),
                    "max": max(v.get("max", 0) for v in values),
                }

        # Checks: sum passes and fails
        check_values = [m["checks"] for m in all_metrics if "checks" in m]
        if check_values:
            total_passes = sum(v.get("passes", 0) for v in check_values)
            total_fails = sum(v.get("fails", 0) for v in check_values)
            total = total_passes + total_fails
            merged["checks"] = {
                "passes": total_passes,
                "fails": total_fails,
                "value": (total_passes / total * 100) if total > 0 else 0,
            }

        return merged

    @staticmethod
    def _metric_counter(metrics: Dict[str, Any], key: str) -> Optional[Dict[str, float]]:
        item = metrics.get(key)
        if not isinstance(item, dict):
            return None
        count_val = item.get("count")
        rate_val = item.get("rate")
        count = float(count_val) if isinstance(count_val, (int, float)) else 0.0
        rate = float(rate_val) if isinstance(rate_val, (int, float)) else 0.0
        return {"count": count, "rate": rate}

    @staticmethod
    def _metric_trend(metrics: Dict[str, Any], key: str) -> Optional[Dict[str, float]]:
        item = metrics.get(key)
        if not isinstance(item, dict):
            return None
        def _num(name: str) -> float:
            val = item.get(name)
            return float(val) if isinstance(val, (int, float)) else 0.0
        return {
            "avg": _num("avg"),
            "med": _num("med"),
            "p90": _num("p(90)"),
            "p95": _num("p(95)"),
            "p99": _num("p(99)"),
            "min": _num("min"),
            "max": _num("max"),
        }

    @staticmethod
    def _parse_k6_json(path: Path) -> Dict[str, Any]:
        if not path.is_file():
            return {}
        try:
            with open(path) as fp:
                data = json.load(fp)
            metrics = data.get("metrics", {})
            result: Dict[str, Any] = {}
            for key in (
                "ws_messages_sent",
                "ws_messages_received",
                "ws_msgs_sent",
                "ws_msgs_received",
                "ws_pings_sent",
                "ws_pongs_received",
                "ws_connections_opened",
                "ws_connections_closed",
                "ws_sessions",
                "iterations",
                "data_sent",
                "data_received",
            ):
                counter = WsBenchmarkRunner._metric_counter(metrics, key)
                if counter:
                    result[key] = counter

            for key in (
                "ws_echo_rtt_ms",
                "ws_ping_rtt_ms",
                "ws_connection_lifetime_ms",
                "ws_connecting",
                "ws_session_duration",
                "iteration_duration",
            ):
                trend = WsBenchmarkRunner._metric_trend(metrics, key)
                if trend:
                    result[key] = trend

            checks = metrics.get("checks")
            if isinstance(checks, dict):
                result["checks"] = {
                    "passes": float(checks.get("passes", 0) or 0),
                    "fails": float(checks.get("fails", 0) or 0),
                    "value": float(checks.get("value", 0) or 0),
                }

            return result
        except (json.JSONDecodeError, KeyError):
            return {}

    @staticmethod
    def _primary_latency(metrics: Dict[str, Any]) -> Optional[Dict[str, float]]:
        for key in (
            "ws_echo_rtt_ms",
            "ws_ping_rtt_ms",
            "ws_connection_lifetime_ms",
            "ws_connecting",
            "ws_session_duration",
        ):
            value = metrics.get(key)
            if isinstance(value, dict):
                return value
        return None

    @staticmethod
    def _primary_throughput_rate(metrics: Dict[str, Any]) -> Optional[float]:
        for key in (
            "ws_messages_sent",
            "ws_msgs_sent",
            "ws_messages_received",
            "ws_msgs_received",
            "ws_pings_sent",
            "ws_sessions",
        ):
            value = metrics.get(key)
            if isinstance(value, dict):
                rate = value.get("rate")
                if isinstance(rate, (int, float)):
                    return float(rate)
        return None

    # ----------------------- websocket-bench execution --------------------- #

    def _run_ws_bench(self, server: str) -> RunResult:
        port = SERVER_PORTS[server]
        ws_url = f"ws://127.0.0.1:{port}/ws-uncompressed"

        cmd = [
            "websocket-bench", "-c", str(self.vus),
            "-d", self.duration,
            "-s", "128",  # 128-byte message payload
            ws_url,
        ]

        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            output = result.stdout + result.stderr
            metrics = self._parse_ws_bench_output(output)
            return RunResult(
                scenario="raw-throughput",
                server=server,
                tool="websocket-bench",
                metrics=metrics,
                raw_output=output,
                success=result.returncode == 0,
            )
        except subprocess.TimeoutExpired:
            return RunResult(
                scenario="raw-throughput",
                server=server,
                tool="websocket-bench",
                success=False,
                raw_output="websocket-bench timed out",
            )
        except FileNotFoundError:
            return RunResult(
                scenario="raw-throughput",
                server=server,
                tool="websocket-bench",
                success=False,
                raw_output="websocket-bench not found. Install from https://github.com/matttomasetti/websocket-bench",
            )

    @staticmethod
    def _parse_ws_bench_output(output: str) -> Dict[str, Any]:
        metrics: Dict[str, Any] = {}
        # Try to extract messages/sec from typical websocket-bench output
        for line in output.splitlines():
            lower = line.lower()
            if "messages/sec" in lower or "msg/s" in lower:
                nums = re.findall(r"[\d,.]+", line)
                if nums:
                    try:
                        metrics["messages_per_sec"] = float(nums[-1].replace(",", ""))
                    except ValueError:
                        pass
            if "connections" in lower and "established" in lower:
                nums = re.findall(r"\d+", line)
                if nums:
                    metrics["connections_established"] = int(nums[0])
        return metrics

    # ----------------------- Main workflow --------------------------------- #

    def run(self) -> None:
        # Verify tools
        k6_available = shutil.which("k6") is not None
        ws_bench_available = shutil.which("websocket-bench") is not None

        if not k6_available:
            print("WARNING: k6 not found. Install: https://k6.io/docs/get-started/installation/")
            print("         k6 scenarios will be skipped.\n")

        if self.enable_ws_bench and not ws_bench_available:
            print("WARNING: websocket-bench not found. Raw throughput tests will be skipped.\n")

        print(f"WebSocket Benchmarks")
        print(f"  Servers:   {', '.join(self.servers_to_test)}")
        print(f"  Scenarios: {', '.join(self.scenarios_to_test)}")
        print(f"  VUs: {self.vus}, Duration: {self.duration}")
        print(f"  k6 instances: {self.k6_instances}, Pipeline depth: {self.pipeline_depth}")
        print(f"  Results:   {self.output_dir}\n")

        try:
            for server in self.servers_to_test:
                self._run_server_suite(server, k6_available, ws_bench_available)
        finally:
            self._stop_all()

        self._print_results()
        self._write_json()
        self._write_badge()
        self._write_html()
        print(f"\nResults saved to {self.result_file}")
        print(f"JSON summary: {self.json_file}")
        if self.html_file.exists():
            print(f"HTML report:  {self.html_file}")

        # Fail CI if aeronet had any errors
        if self._aeronet_errors_found:
            print("\nWebSocket benchmarks complete (aeronet reported errors)")
            sys.exit(1)

    def _run_server_suite(
        self, server: str, k6_ok: bool, ws_bench_ok: bool
    ) -> None:
        print(f"\n{'=' * 50}")
        print(f"  Server: {server}")
        print(f"{'=' * 50}")

        if not self._start_server(server):
            print(f"  SKIP: {server} failed to start")
            return

        # Warmup
        if self.warmup != "0s" and k6_ok and self.scenarios_to_test:
            print(f"  Warming up ({self.warmup})...")
            first_scenario = self.scenarios_to_test[0]
            script = self.script_dir / K6_SCENARIOS[first_scenario]
            if script.is_file():
                port = SERVER_PORTS[server]
                env = os.environ.copy()
                env.update({
                    "WS_URL": f"ws://127.0.0.1:{port}/ws-uncompressed",
                    "VUS": str(max(1, self.vus // 4)),
                    "DURATION": self.warmup,
                    "SESSION_DURATION_MS": "3000",
                })
                subprocess.run(
                    ["k6", "run", "--quiet", str(script)],
                    env=env, capture_output=True, timeout=120,
                )

        # k6 scenarios
        if k6_ok:
            for idx, scenario in enumerate(self.scenarios_to_test):
                # Skip compressed scenarios for servers that don't support it
                if scenario in COMPRESSED_SCENARIOS and server not in COMPRESSION_CAPABLE_SERVERS:
                    print(f"  Skipping k6: {scenario} ({server} does not support WS compression)")
                    continue
                if idx > 0:
                    time.sleep(2)  # Cooldown between scenarios
                print(f"  Running k6: {scenario} ...", end=" ", flush=True)
                result = self._run_k6(server, scenario)
                self.results.append(result)
                if result.success:
                    rtt = self._primary_latency(result.metrics)
                    rate = self._primary_throughput_rate(result.metrics)
                    checks = result.metrics.get("checks", {})
                    fails = checks.get("fails", 0) if isinstance(checks, dict) else 0
                    parts: List[str] = []
                    if rtt:
                        parts.append(f"p95={rtt.get('p95', 0):.3f}ms")
                    if isinstance(rate, (int, float)):
                        parts.append(f"rate={rate:,.0f}/s")
                    if isinstance(fails, (int, float)) and fails > 0:
                        parts.append(f"fails={int(fails)}")
                    print(", ".join(parts) if parts else "OK")

                    # Check for aeronet errors
                    if server == "aeronet" and isinstance(fails, (int, float)) and fails > 0:
                        self._aeronet_errors_found = True
                        print(
                            f"  ERROR: aeronet reported {int(fails)} check failures "
                            f"for scenario '{scenario}'"
                        )
                else:
                    print("FAILED")
                    if server == "aeronet":
                        self._aeronet_errors_found = True
                        print(
                            f"  ERROR: aeronet k6 run failed for scenario '{scenario}'"
                        )

        # websocket-bench raw throughput
        if self.enable_ws_bench and ws_bench_ok:
            print(f"  Running websocket-bench: raw-throughput ...", end=" ", flush=True)
            result = self._run_ws_bench(server)
            self.results.append(result)
            mps = result.metrics.get("messages_per_sec")
            if mps:
                print(f"{mps:,.0f} msg/s")
            elif result.success:
                print("OK")
            else:
                print("FAILED")

        self._stop_server(server)
        time.sleep(1)

    # ----------------------- Output --------------------------------------- #

    def _build_results_dicts(self) -> tuple:
        """Build (rps_dict, latency_dict) keyed by (server, scenario) for TablePrinter."""
        rps_data: Dict[tuple, str] = {}
        latency_data: Dict[tuple, str] = {}
        for res in self.results:
            if not res.success:
                continue
            key = (res.server, res.scenario)
            rate = self._primary_throughput_rate(res.metrics)
            rtt = self._primary_latency(res.metrics)
            if isinstance(rate, (int, float)):
                rps_data[key] = f"{int(rate):,}"
            if rtt:
                p95 = rtt.get("p95", 0)
                if isinstance(p95, (int, float)):
                    latency_data[key] = f"{p95:.3f}ms"
        return rps_data, latency_data

    def _print_results(self) -> None:
        from bench_utils import TablePrinter  # noqa: local import

        rps_data, latency_data = self._build_results_dicts()

        # Only include k6 scenarios in the table
        scenarios = []
        for res in self.results:
            if res.scenario not in scenarios:
                scenarios.append(res.scenario)

        printer = TablePrinter(
            servers=self.servers_to_test,
            scenarios=scenarios,
            metrics=[
                (
                    "WEBSOCKET THROUGHPUT",
                    "(Messages/sec – higher is better)",
                    rps_data,
                    True,
                ),
                (
                    "WEBSOCKET LATENCY",
                    "(p95 RTT – lower is better)",
                    latency_data,
                    False,
                ),
            ],
        )
        printer.print_all()

        # Write to file
        servers = self.servers_to_test
        with open(self.result_file, "w") as fp:
            fp.write(f"WebSocket Benchmark Results\n")
            fp.write(f"Date: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
            fp.write(f"Servers: {', '.join(servers)}\n")
            fp.write(f"VUs: {self.vus}, Duration: {self.duration}\n\n")
            for res in self.results:
                fp.write(f"[{res.tool}] {res.server} / {res.scenario}: ")
                fp.write(json.dumps(res.metrics) + "\n")

    def _write_html(self) -> None:
        renderer = self.script_dir / "render_benchmarks_html.py"
        if not renderer.is_file():
            print("WARNING: render_benchmarks_html.py not found, skipping HTML report generation")
            return
        try:
            subprocess.run(
                [
                    sys.executable,
                    str(renderer),
                    "--input",
                    str(self.json_file),
                    "--output",
                    str(self.html_file),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
        except subprocess.CalledProcessError as exc:
            print("WARNING: failed to generate WS HTML report")
            if exc.stderr:
                print(exc.stderr.strip())

    def _write_json(self) -> None:
        # Build unified dict-based results (same structure as HTTP benchmarks)
        results_dict: Dict[str, Any] = {}
        for res in self.results:
            entry = results_dict.setdefault(res.scenario, {
                "rps": {},
                "latency": {},
            })
            if not res.success:
                continue
            rate = self._primary_throughput_rate(res.metrics)
            rtt = self._primary_latency(res.metrics)
            if isinstance(rate, (int, float)):
                entry["rps"][res.server] = round(rate, 1)
            if rtt:
                p95 = rtt.get("p95")
                if isinstance(p95, (int, float)):
                    entry["latency"][res.server] = f"{p95:.3f}ms"

        # Collect unique scenarios in order
        scenarios: List[str] = []
        for res in self.results:
            if res.scenario not in scenarios:
                scenarios.append(res.scenario)

        data = {
            "benchmark_type": "websocket",
            "tool": "k6",
            "threads": self.args.threads,
            "vus": self.vus,
            "duration": self.duration,
            "warmup": self.warmup,
            "servers": self.servers_to_test,
            "scenarios": scenarios,
            "results": results_dict,
        }
        with open(self.json_file, "w") as fp:
            json.dump(data, fp, indent=2)

        # Write a stable "latest" symlink / copy for CI artifact upload
        latest = self.output_dir / "ws_benchmark_latest.json"
        try:
            latest.unlink(missing_ok=True)
            latest.symlink_to(self.json_file.name)
        except OSError:
            import shutil as _sh
            _sh.copy2(self.json_file, latest)

    # ----------------------- Badge ---------------------------------------- #

    def _write_badge(self) -> None:
        """Write a Shields.io endpoint-badge JSON for the best aeronet throughput."""
        best_rate = 0.0
        best_scenario = None
        for res in self.results:
            if res.server != "aeronet" or not res.success:
                continue
            rate = self._primary_throughput_rate(res.metrics)
            if isinstance(rate, (int, float)) and rate > best_rate:
                best_rate = rate
                best_scenario = res.scenario

        if best_scenario is None or best_rate <= 0:
            return

        badge = {
            "schemaVersion": 1,
            "label": "ws aeronet peak msg/s",
            "message": f"{self._format_badge_value(best_rate)} msg/s",
            "color": self._badge_color(best_rate),
            "labelColor": "#0f172a",
            "namedLogo": "speedtest",
            "cacheSeconds": 3600,
        }
        badge_path = self.output_dir / "ws_benchmark_badge.json"
        with badge_path.open("w", encoding="utf-8") as fp:
            json.dump(badge, fp, indent=2)

    @staticmethod
    def _format_badge_value(value: float) -> str:
        if value >= 1_000_000:
            return f"{value / 1_000_000:.1f}M"
        if value >= 1_000:
            return f"{value / 1_000:.0f}k"
        return f"{value:.0f}"

    @staticmethod
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


# ----------------------------- CLI ---------------------------------------- #


def parse_args() -> argparse.Namespace:
    cpu_count = os.cpu_count() or 1
    default_threads = max(1, cpu_count // 2)
    # WS workloads need substantial concurrency to saturate server threads.
    # Keep a floor above tiny runs while scaling with thread count.
    default_vus = max(100, default_threads * 64)
    # Run enough k6 instances to use remaining CPU cores.
    default_k6_instances = max(1, min(cpu_count - default_threads, 4))

    parser = argparse.ArgumentParser(
        description="Run WebSocket benchmarks across frameworks using k6 and websocket-bench"
    )
    parser.add_argument(
        "--server", default="all",
        help=f"Comma-separated servers ({','.join(SERVER_ORDER)}) or 'all'",
    )
    parser.add_argument(
        "--scenario", default="all",
        help=f"Comma-separated scenarios ({','.join(K6_SCENARIOS)}) or 'all'",
    )
    parser.add_argument(
        "--vus",
        type=int,
        default=default_vus,
        help=(
            "Virtual users (k6) / connections "
            f"(default: {default_vus}, derived from --threads={default_threads})"
        ),
    )
    parser.add_argument("--duration", default="30s", help="Test duration per scenario")
    parser.add_argument("--warmup", default="5s", help="Warmup duration per server")
    parser.add_argument("--session-duration-ms", type=int, default=10000, help="WS session lifetime in ms")
    parser.add_argument("--threads", type=int, default=default_threads, help="Server worker threads")
    parser.add_argument("--output", default="./ws-results", help="Output directory")
    parser.add_argument(
        "--websocket-bench", action="store_true", default=False,
        help="Also run websocket-bench for raw throughput measurement",
    )
    parser.add_argument(
        "--k6-instances",
        type=int,
        default=default_k6_instances,
        help=(
            "Parallel k6 processes to maximise client-side throughput "
            f"(default: {default_k6_instances})"
        ),
    )
    parser.add_argument(
        "--pipeline-depth",
        type=int,
        default=1,
        help=(
            "In-flight messages per VU (0 = timer mode, >0 = send-on-receive). "
            "Higher values better saturate server threads (default: 1)"
        ),
    )
    parser.add_argument(
        "--smoke", action="store_true", default=False,
        help="Quick smoke run (5s, 10 VUs) to validate setup",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.smoke:
        args.vus = 10
        args.duration = "5s"
        args.warmup = "2s"
        args.session_duration_ms = 3000
        args.k6_instances = 1
        args.pipeline_depth = 0

    runner = WsBenchmarkRunner(args)
    try:
        runner.run()
    except WsBenchmarkError as exc:
        print(f"ERROR: {exc}")
        sys.exit(1)


if __name__ == "__main__":
    main()
