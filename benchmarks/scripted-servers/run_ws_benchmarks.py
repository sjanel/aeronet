#!/usr/bin/env python3
"""WebSocket benchmark orchestration script.

Launches C++ benchmark servers, drives them with the native ws-loadgen load
generator (or k6, and optionally websocket-bench), and produces a unified
results summary (text + JSON + HTML).

The server and the load generator are pinned to disjoint CPUs and the CPU
utilization of both is measured: a server below SATURATION_THRESHOLD_PCT of
its CPUs was not the bottleneck (load generator, kernel or latency bound), so
its number does not measure it, and it is flagged as such. ws-loadgen is the
default because k6 spends several times more CPU per message than the servers.

Usage:
    ./run_ws_benchmarks.py [options]
    ./run_ws_benchmarks.py --server aeronet,drogon --scenario echo-small,churn
    ./run_ws_benchmarks.py --tool k6   # Use k6 instead of ws-loadgen
    ./run_ws_benchmarks.py --smoke     # Quick validation run (5s, 10 connections)
"""
from __future__ import annotations

import argparse
import json
import os
import re
import resource
import shutil
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

from bench_utils import (
    SATURATION_THRESHOLD_PCT,
    CpuMeter,
    CpuUsage,
    available_cpus,
    duration_to_seconds,
    format_pct,
    plan_server_benchmark_cpus,
)

# ----------------------------- Constants ----------------------------------- #

K6_SCENARIOS: Dict[str, str] = {
    "echo-small": "k6/ws_echo_small.js",
    "echo-medium": "k6/ws_echo_medium.js",
    "echo-large": "k6/ws_echo_large.js",
    "mix": "k6/ws_mix_text_binary.js",
    "ping-pong": "k6/ws_ping_pong.js",
    "churn": "k6/ws_churn.js",
    "compression": "k6/ws_compression.js",
}

# ws-loadgen arguments reproducing each k6 scenario (same payloads and message types).
LOADGEN_SCENARIOS: Dict[str, List[str]] = {
    "echo-small": ["--mode", "echo", "--payload-size", "128"],
    "echo-medium": ["--mode", "echo", "--payload-size", "2048"],
    "echo-large": ["--mode", "echo", "--payload-size", "65536", "--binary"],
    "mix": ["--mode", "mix"],
    "ping-pong": ["--mode", "ping"],
    "churn": ["--mode", "churn"],
    "compression": ["--mode", "echo", "--json-payload", "--compress"],
}

# Large (64 KB) messages are kept at one in flight per connection: deeper pipelines
# only grow the socket buffers without adding load.
SINGLE_MESSAGE_PIPELINE_SCENARIOS = {"echo-large"}

LOADGEN_BINARY = "ws-loadgen"
TOOLS = ("auto", LOADGEN_BINARY, "k6")

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
    "beast": 8089,
}

SERVER_ORDER = ["aeronet", "uwebsockets", "drogon", "beast"]


@dataclass
class RunResult:
    scenario: str
    server: str
    tool: str  # "ws-loadgen", "k6" or "websocket-bench"
    metrics: Dict[str, Any] = field(default_factory=dict)
    raw_output: str = ""
    success: bool = True
    cpu: Optional[CpuUsage] = None


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
        self.tool = self._resolve_tool(args.tool)

        # Server worker threads. The load generator is sized and placed separately, on
        # CPUs disjoint from the server's, so that it can always saturate the server.
        self.server_threads = max(1, args.threads)
        self._cpu_pin_enabled = (
            not args.no_cpu_pin and sys.platform.startswith("linux") and shutil.which("taskset") is not None
        )
        self.cpu_plan = plan_server_benchmark_cpus(
            self.server_threads, args.loadgen_threads, pin=self._cpu_pin_enabled
        )
        self.loadgen_threads = self.cpu_plan.loadgen_threads

        self.output_dir = Path(args.output).resolve()
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.logs_dir = self.output_dir / "logs"
        self.logs_dir.mkdir(exist_ok=True)
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        self.result_file = self.output_dir / f"ws_benchmark_{timestamp}.txt"
        self.json_file = self.output_dir / f"ws_benchmark_{timestamp}.json"
        self.html_file = self.output_dir / f"ws_benchmark_{timestamp}.html"
        # Human-readable date of the benchmark run ("date of the shoot"), embedded in
        # the JSON summary and surfaced in the rendered HTML report.
        self.run_datetime = time.strftime("%Y-%m-%d %H:%M:%S %Z")

        self.servers_to_test = self._resolve_servers(args.server)
        self.scenarios_to_test = self._resolve_scenarios(args.scenario)
        self.enable_ws_bench = args.websocket_bench

        self.server_processes: Dict[str, subprocess.Popen] = {}
        self._server_log_fps: Dict[str, Any] = {}
        self.results: List[RunResult] = []

        # Track whether any aeronet scenario reported errors (fails CI)
        self._aeronet_errors_found: bool = False

    # ----------------------- Setup helpers --------------------------------- #

    def _loadgen_binary(self) -> Path:
        return self.build_dir / LOADGEN_BINARY

    def _resolve_tool(self, tool_arg: str) -> str:
        """Load generator to use: ws-loadgen when built ('auto'), k6 otherwise."""
        if tool_arg not in TOOLS:
            raise WsBenchmarkError(f"Unknown tool: {tool_arg}. Available: {', '.join(TOOLS)}")
        if tool_arg == "k6":
            return "k6"
        if self._loadgen_binary().is_file():
            return LOADGEN_BINARY
        if tool_arg == LOADGEN_BINARY:
            raise WsBenchmarkError(
                f"{LOADGEN_BINARY} not found in {self.build_dir} (build the '{LOADGEN_BINARY}' target)"
            )
        print(f"WARNING: {LOADGEN_BINARY} not found in {self.build_dir}, falling back to k6")
        return "k6"

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
    def _is_port_in_use(port: int) -> bool:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            return False

    @classmethod
    def _wait_for_port(cls, port: int, timeout: float = 10.0, *, proc: Optional[subprocess.Popen] = None) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if proc is not None and proc.poll() is not None:
                return False
            if cls._is_port_in_use(port):
                return True
            time.sleep(0.1)
        return False

    # ----------------------- Server lifecycle ------------------------------ #

    def _start_server(self, name: str) -> bool:
        if name in self.server_processes:
            return True
        port = SERVER_PORTS[name]
        if self._is_port_in_use(port):
            print(
                f"  ERROR: port {port} is already in use before starting {name}; "
                "stop the stale process and retry"
            )
            return False
        binary = self._server_binary(name)
        cmd = [
            *self.cpu_plan.server_prefix(),
            str(binary), "--port", str(port), "--threads", str(self.server_threads),
        ]
        log_path = self.logs_dir / f"{name}_server.log"
        log_fp = open(log_path, "w")

        def _preexec() -> None:
            os.setsid()
            soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            if soft < hard:
                resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))

        proc = subprocess.Popen(
            cmd, stdout=log_fp, stderr=subprocess.STDOUT, preexec_fn=_preexec
        )
        self.server_processes[name] = proc
        self._server_log_fps[name] = log_fp
        if not self._wait_for_port(port, proc=proc):
            if proc.poll() is not None:
                print(f"  ERROR: {name} exited during startup (exit={proc.returncode}); see {log_path}")
            else:
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
        log_fp = self._server_log_fps.pop(name, None)
        if log_fp:
            log_fp.close()

    def _stop_all(self) -> None:
        for name in list(self.server_processes):
            self._stop_server(name)

    def _server_log_path(self, name: str) -> Path:
        return self.logs_dir / f"{name}_server.log"

    @staticmethod
    def _tail_file(path: Path, max_lines: int = 80) -> List[str]:
        if not path.is_file():
            return []
        try:
            with open(path, encoding="utf-8", errors="replace") as fp:
                lines = fp.read().splitlines()
            if not lines:
                return []
            return lines[-max_lines:]
        except OSError:
            return []

    @staticmethod
    def _interesting_output_lines(output: str, max_lines: int = 20) -> List[str]:
        if not output.strip():
            return []
        interesting: List[str] = []
        pattern = re.compile(r"(warn|error|fail|panic|exception|timeout|refused|status\s*!=\s*101|status\s*===\s*101)", re.IGNORECASE)
        for line in output.splitlines():
            if pattern.search(line):
                interesting.append(line)
        if interesting:
            return interesting[-max_lines:]
        return output.splitlines()[-max_lines:]

    def _print_error_context(self, server: str, scenario: str, result: RunResult) -> None:
        print("  --- diagnostics begin ---")
        checks = result.metrics.get("checks", {})
        if isinstance(checks, dict):
            passes = checks.get("passes", 0)
            fails = checks.get("fails", 0)
            value = checks.get("value", 0)
            if isinstance(passes, (int, float)) and isinstance(fails, (int, float)):
                print(
                    "  checks summary: "
                    f"passes={int(passes)}, fails={int(fails)}, success={float(value):.2f}%"
                )

        tool_log_path = self.logs_dir / f"{result.tool}_{server}_{scenario}.log"
        print(f"  {result.tool} raw log: {tool_log_path}")
        for line in self._interesting_output_lines(result.raw_output):
            print(f"    {result.tool}> {line}")

        server_log = self._server_log_path(server)
        print(f"  server log tail: {server_log}")
        tail_lines = self._tail_file(server_log)
        if tail_lines:
            for line in tail_lines:
                print(f"    srv> {line}")
        else:
            print("    srv> <no server log output>")
        print("  --- diagnostics end ---")

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
        if scenario == "echo-large":
            # Large (64 KB) payloads in tight pipeline mode can create extreme
            # allocation pressure and make results unstable. Keep this scenario
            # in timer mode unless explicitly overridden by env.
            base_env["PIPELINE_DEPTH"] = "0"
        elif self.pipeline_depth > 0:
            base_env["PIPELINE_DEPTH"] = str(self.pipeline_depth)

        # Spread the load generator CPUs over the k6 instances so k6 doesn't steal server CPU
        gomaxprocs = max(2, self.loadgen_threads // n_instances)
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
                *self.cpu_plan.loadgen_prefix(),
                "k6", "run",
                "--address", "127.0.0.1:0",
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
        for idx, proc in enumerate(procs):
            try:
                stdout, stderr = proc.communicate(timeout=300)
                combined_output = stdout.decode(errors="replace") + stderr.decode(errors="replace")
                outputs.append(
                    f"=== k6 instance {idx} (exit={proc.returncode}) ===\n{combined_output}".rstrip()
                )
                if proc.returncode != 0:
                    all_success = False
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                outputs.append(f"=== k6 instance {idx} (exit=timeout) ===\nk6 instance timed out after 300s")
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

        raw_log_path = self.logs_dir / f"k6_{server}_{scenario}.log"
        try:
            with open(raw_log_path, "w", encoding="utf-8") as fp:
                fp.write("\n\n".join(outputs) if outputs else "<no k6 output>\n")
        except OSError:
            pass

        return RunResult(
            scenario=scenario,
            server=server,
            tool="k6",
            metrics=metrics,
            raw_output="\n\n".join(outputs),
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

    # ----------------------- ws-loadgen execution -------------------------- #

    def _loadgen_command(self, server: str, scenario: str) -> List[str]:
        connections = max(1, self.vus)
        ws_path = "/ws-compressed" if scenario in COMPRESSED_SCENARIOS else "/ws-uncompressed"
        pipeline = 1 if scenario in SINGLE_MESSAGE_PIPELINE_SCENARIOS else max(1, self.pipeline_depth)
        return [
            *self.cpu_plan.loadgen_prefix(),
            str(self._loadgen_binary()),
            "--port", str(SERVER_PORTS[server]),
            "--path", ws_path,
            "--connections", str(connections),
            # Each thread needs at least one connection.
            "--threads", str(max(1, min(self.loadgen_threads, connections))),
            "--duration", self.duration,
            "--warmup", self.warmup,
            "--pipeline", str(pipeline),
            *LOADGEN_SCENARIOS[scenario],
        ]

    def _run_loadgen(self, server: str, scenario: str) -> RunResult:
        cmd = self._loadgen_command(server, scenario)
        failure = RunResult(scenario=scenario, server=server, tool=LOADGEN_BINARY, success=False)
        warmup_s = duration_to_seconds(self.warmup) or 0.0
        timeout_s = warmup_s + (duration_to_seconds(self.duration) or 0.0) + 60.0
        meter = self._cpu_meter(server)
        try:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        except OSError as exc:
            failure.raw_output = f"failed to launch {LOADGEN_BINARY}: {exc}"
            return failure
        # ws-loadgen connects and warms up before measuring: measure the server CPU over the same window.
        time.sleep(warmup_s)
        meter.start()
        try:
            stdout, stderr = proc.communicate(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()
            failure.raw_output = f"{LOADGEN_BINARY} timed out after {timeout_s:.0f}s\n{stdout}{stderr}"
            return failure
        usage = meter.stop()
        output = f"$ {' '.join(cmd)}\n{stdout}{stderr}"
        try:
            with open(self.logs_dir / f"{LOADGEN_BINARY}_{server}_{scenario}.log", "w", encoding="utf-8") as fp:
                fp.write(output)
        except OSError:
            pass

        data = self._parse_loadgen_output(stdout)
        if proc.returncode != 0 or data is None:
            failure.raw_output = output
            return failure
        # The generator reports its own CPU time over the measurement window (warmup excluded).
        cpu_s, duration_s = data.get("cpu_s"), data.get("duration_s")
        if isinstance(cpu_s, (int, float)) and isinstance(duration_s, (int, float)) and duration_s > 0:
            usage.loadgen_pct = 100.0 * cpu_s / (duration_s * self._loadgen_cpu_count())
        with open(self.output_dir / f"{LOADGEN_BINARY}_{server}_{scenario}.json", "w", encoding="utf-8") as fp:
            json.dump(data, fp, indent=2)
        return RunResult(
            scenario=scenario,
            server=server,
            tool=LOADGEN_BINARY,
            metrics=self._loadgen_metrics(scenario, data),
            raw_output=output,
            cpu=usage,
        )

    @staticmethod
    def _parse_loadgen_output(stdout: str) -> Optional[Dict[str, Any]]:
        """The ws-loadgen result: the last JSON object line of its output."""
        for line in reversed(stdout.splitlines()):
            line = line.strip()
            if line.startswith("{"):
                try:
                    data = json.loads(line)
                except json.JSONDecodeError:
                    return None
                return data if isinstance(data, dict) else None
        return None

    @staticmethod
    def _loadgen_metrics(scenario: str, data: Dict[str, Any]) -> Dict[str, Any]:
        """Convert a ws-loadgen result to the (k6) metric names used by the reports."""

        def number(source: Dict[str, Any], key: str) -> float:
            value = source.get(key)
            return float(value) if isinstance(value, (int, float)) else 0.0

        latency_us = data.get("latency_us")
        latency_us = latency_us if isinstance(latency_us, dict) else {}
        latency_ms = {
            name: number(latency_us, key) / 1000.0
            for name, key in (("avg", "avg"), ("med", "p50"), ("p90", "p90"), ("p95", "p95"),
                              ("p99", "p99"), ("max", "max"))
        }
        if scenario == "churn":
            counter_key, latency_key, count_key, rate_key = (
                "ws_sessions", "ws_connection_lifetime_ms", "sessions", "sessions_rate")
        elif scenario == "ping-pong":
            counter_key, latency_key, count_key, rate_key = ("ws_pings_sent", "ws_ping_rtt_ms", "messages", "rate")
        else:
            counter_key, latency_key, count_key, rate_key = ("ws_messages_sent", "ws_echo_rtt_ms", "messages", "rate")
        passes = number(data, count_key)
        # A refused permessage-deflate offer means the run did not measure what it claims.
        fails = number(data, "errors") + number(data, "compression_refused")
        total = passes + fails
        return {
            counter_key: {"count": passes, "rate": number(data, rate_key)},
            latency_key: latency_ms,
            "checks": {"passes": passes, "fails": fails, "value": 100.0 * passes / total if total > 0 else 0.0},
        }

    # ----------------------- CPU utilization ------------------------------- #

    def _server_cpu_count(self) -> int:
        return len(self.cpu_plan.server_cpus) or self.server_threads

    def _loadgen_cpu_count(self) -> int:
        return len(self.cpu_plan.loadgen_cpus) or self.loadgen_threads

    def _cpu_meter(self, server: str) -> CpuMeter:
        proc = self.server_processes.get(server)
        return CpuMeter(
            server_pid=proc.pid if proc is not None else None,
            server_cpus=self._server_cpu_count(),
            loadgen_cpus=self._loadgen_cpu_count(),
        )

    def _print_cpu_usage(self, server: str, scenario: str, usage: CpuUsage) -> None:
        print(
            f"    CPU: server {format_pct(usage.server_pct)} of {self._server_cpu_count()} CPU(s), "
            f"load generator {format_pct(usage.loadgen_pct)} of {self._loadgen_cpu_count()} CPU(s)"
        )
        if usage.server_saturated() is False:
            print(
                f"    WARNING: {server} is not saturated on '{scenario}' (below "
                f"{SATURATION_THRESHOLD_PCT:.0f}% of its CPUs): the server was not the bottleneck, "
                "this does not measure its throughput"
            )

    def _print_saturation_summary(self) -> None:
        """List the server CPU utilization per scenario and flag the measurements where it was not the bottleneck."""
        measured = [res for res in self.results if res.success and res.cpu is not None]
        if not measured:
            return
        print(
            f"\nSERVER CPU UTILIZATION (% of its {self._server_cpu_count()} CPU(s); below "
            f"{SATURATION_THRESHOLD_PCT:.0f}% = not saturated: the load generator, the kernel or latency was the "
            "bottleneck)"
        )
        unsaturated: List[str] = []
        for scenario in self.scenarios_to_test:
            cells = []
            for res in measured:
                if res.scenario != scenario:
                    continue
                not_saturated = res.cpu.server_saturated() is False
                cells.append(f"{res.server}={format_pct(res.cpu.server_pct)}{'!' if not_saturated else ''}")
                if not_saturated:
                    unsaturated.append(f"{res.server}/{scenario}")
            if cells:
                print(f"  {scenario:<12} " + "  ".join(cells))
        if unsaturated:
            print(f"WARNING: not saturated server measurements: {', '.join(unsaturated)}")

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

        if self.tool == "k6" and not k6_available:
            print("WARNING: k6 not found. Install: https://k6.io/docs/get-started/installation/")
            print("         k6 scenarios will be skipped.\n")

        if self.enable_ws_bench and not ws_bench_available:
            print("WARNING: websocket-bench not found. Raw throughput tests will be skipped.\n")

        print(f"WebSocket Benchmarks")
        print(f"  Servers:   {', '.join(self.servers_to_test)}")
        print(f"  Scenarios: {', '.join(self.scenarios_to_test)}")
        print(f"  Tool: {self.tool}, connections: {self.vus}, duration: {self.duration}, warmup: {self.warmup}")
        if self.tool == "k6":
            print(f"  k6 instances: {self.k6_instances}, Pipeline depth: {self.pipeline_depth}")
        else:
            print(f"  Pipeline depth: {max(1, self.pipeline_depth)}")
        print(f"  Server threads: {self.server_threads}, {self.cpu_plan.describe()}")
        print(f"  Results:   {self.output_dir}\n")

        try:
            for server in self.servers_to_test:
                self._run_server_suite(server, k6_available, ws_bench_available)
        finally:
            self._stop_all()

        self._print_results()
        self._print_saturation_summary()
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

    def _warmup_k6(self, server: str) -> None:
        """Warm the server up before the k6 scenarios (ws-loadgen warms up within each scenario)."""
        if self.warmup == "0s" or not self.scenarios_to_test:
            return
        print(f"  Warming up ({self.warmup})...")
        script = self.script_dir / K6_SCENARIOS[self.scenarios_to_test[0]]
        if not script.is_file():
            return
        env = os.environ.copy()
        env.update({
            "WS_URL": f"ws://127.0.0.1:{SERVER_PORTS[server]}/ws-uncompressed",
            "VUS": str(max(1, self.vus // 4)),
            "DURATION": self.warmup,
            "SESSION_DURATION_MS": "3000",
        })
        subprocess.run(
            [*self.cpu_plan.loadgen_prefix(), "k6", "run", "--quiet", str(script)],
            env=env, capture_output=True, timeout=120,
        )

    def _run_scenario(self, server: str, scenario: str) -> RunResult:
        if self.tool == LOADGEN_BINARY:
            return self._run_loadgen(server, scenario)
        # k6 instances are waited for before the meter stops: their CPU time counts as load generator usage.
        meter = self._cpu_meter(server)
        meter.start()
        result = self._run_k6(server, scenario)
        result.cpu = meter.stop()
        return result

    def _report_scenario_result(self, server: str, scenario: str, result: RunResult) -> None:
        """Print a scenario outcome and record aeronet errors (which fail the CI run)."""
        if not result.success:
            print("FAILED")
            self._print_error_context(server, scenario, result)
            if server == "aeronet":
                self._aeronet_errors_found = True
                print(f"  ERROR: aeronet {result.tool} run failed for scenario '{scenario}'")
            return
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
        if result.cpu is not None:
            self._print_cpu_usage(server, scenario, result.cpu)

        if isinstance(fails, (int, float)) and fails > 0:
            self._print_error_context(server, scenario, result)
            if server == "aeronet":
                self._aeronet_errors_found = True
                print(f"  ERROR: aeronet reported {int(fails)} check failures for scenario '{scenario}'")

    def _run_server_suite(
        self, server: str, k6_ok: bool, ws_bench_ok: bool
    ) -> None:
        print(f"\n{'=' * 50}")
        print(f"  Server: {server}")
        print(f"{'=' * 50}")

        if not self._start_server(server):
            print(f"  SKIP: {server} failed to start")
            return

        tool_ok = self.tool == LOADGEN_BINARY or k6_ok
        if self.tool == "k6" and k6_ok:
            self._warmup_k6(server)

        if tool_ok:
            for idx, scenario in enumerate(self.scenarios_to_test):
                # Skip compressed scenarios for servers that don't support it
                if scenario in COMPRESSED_SCENARIOS and server not in COMPRESSION_CAPABLE_SERVERS:
                    print(f"  Skipping {scenario} ({server} does not support WS compression)")
                    continue

                proc = self.server_processes.get(server)
                if proc is not None and proc.poll() is not None:
                    print(
                        f"  ERROR: {server} process exited unexpectedly before scenario "
                        f"'{scenario}' (exit={proc.returncode})"
                    )
                    self._print_error_context(
                        server,
                        scenario,
                        RunResult(scenario=scenario, server=server, tool=self.tool, success=False),
                    )
                    if server == "aeronet":
                        self._aeronet_errors_found = True
                    break

                if idx > 0:
                    time.sleep(2)  # Cooldown between scenarios
                print(f"  Running {self.tool}: {scenario} ...", end=" ", flush=True)
                result = self._run_scenario(server, scenario)
                self.results.append(result)
                self._report_scenario_result(server, scenario, result)

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
            fp.write(f"Tool: {self.tool}, connections: {self.vus}, duration: {self.duration}\n")
            fp.write(f"Server threads: {self.server_threads}, CPU plan: {self.cpu_plan.describe()}\n\n")
            for res in self.results:
                fp.write(f"[{res.tool}] {res.server} / {res.scenario}: ")
                fp.write(json.dumps(res.metrics))
                if res.cpu is not None:
                    fp.write(
                        f" (CPU: server {format_pct(res.cpu.server_pct)}, "
                        f"load generator {format_pct(res.cpu.loadgen_pct)})"
                    )
                fp.write("\n")

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
                "server_cpu": {},
                "loadgen_cpu": {},
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
            if res.cpu is not None and res.cpu.server_pct is not None:
                entry["server_cpu"][res.server] = round(res.cpu.server_pct, 1)
            if res.cpu is not None and res.cpu.loadgen_pct is not None:
                entry["loadgen_cpu"][res.server] = round(res.cpu.loadgen_pct, 1)

        # Collect unique scenarios in order
        scenarios: List[str] = []
        for res in self.results:
            if res.scenario not in scenarios:
                scenarios.append(res.scenario)

        data = {
            "benchmark_type": "websocket",
            "tool": self.tool,
            "generated_at": self.run_datetime,
            "threads": self.server_threads,
            "server_threads": self.server_threads,
            "loadgen_threads": self.loadgen_threads,
            "cpu_plan": self.cpu_plan.to_json(),
            "saturation_threshold_pct": SATURATION_THRESHOLD_PCT,
            "vus": self.vus,
            "pipeline_depth": max(1, self.pipeline_depth) if self.tool == LOADGEN_BINARY else self.pipeline_depth,
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
    cpu_count = len(available_cpus())
    # A quarter of the CPUs for the server leaves the rest to the load generator
    # (up to DEFAULT_MAX_LOADGEN_RATIO load generator threads per server thread).
    default_threads = max(1, cpu_count // 4)
    # WS workloads need substantial concurrency to saturate server threads.
    # Keep a floor above tiny runs while scaling with thread count.
    default_vus = max(100, default_threads * 64)
    # Run enough k6 instances to use remaining CPU cores.
    default_k6_instances = max(1, min(cpu_count - default_threads, 4))

    parser = argparse.ArgumentParser(
        description="Run WebSocket benchmarks across frameworks using ws-loadgen (or k6) and websocket-bench"
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
            "Connections (ws-loadgen) / virtual users (k6) "
            f"(default: {default_vus}, derived from --threads={default_threads})"
        ),
    )
    parser.add_argument("--duration", default="30s", help="Test duration per scenario")
    parser.add_argument(
        "--warmup", default="5s",
        help="Warmup duration, before each scenario (ws-loadgen) or once per server (k6)",
    )
    parser.add_argument("--session-duration-ms", type=int, default=10000, help="WS session lifetime in ms (k6)")
    parser.add_argument(
        "--threads", type=int, default=default_threads,
        help=f"Server worker threads (default: {default_threads}, a quarter of the CPUs)",
    )
    parser.add_argument(
        "--tool", default="auto", choices=TOOLS,
        help=f"Load generator: {LOADGEN_BINARY} (native, default when built) or k6",
    )
    parser.add_argument(
        "--loadgen-threads", type=int, default=0,
        help="Load generator threads (default: 0 = all CPUs left by the server, up to 3 per server thread)",
    )
    parser.add_argument(
        "--no-cpu-pin", action="store_true", default=False,
        help="Do not pin the server and the load generator to disjoint CPUs",
    )
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
            "In-flight messages per connection (k6: 0 = timer mode, >0 = send-on-receive; "
            "ws-loadgen: at least 1). Higher values better saturate server threads (default: 1)"
        ),
    )
    parser.add_argument(
        "--smoke", action="store_true", default=False,
        help="Quick smoke run (5s, 10 connections) to validate setup",
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
