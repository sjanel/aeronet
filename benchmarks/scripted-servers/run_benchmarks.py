#!/usr/bin/env python3
"""Benchmark orchestration script for wrk-based HTTP server comparisons."""
from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

# ----------------------------- Data structures ----------------------------- #


@dataclass(frozen=True)
class Scenario:
    name: str
    lua_script: str
    endpoint: str
    requires_restart: bool = False
    requires_static: bool = False
    requires_tls: bool = False
    use_https: bool = False


@dataclass
class ProcessHandle:
    popen: subprocess.Popen
    log_fp: Optional[object]
    log_path: Path
    port: int


@dataclass
class MemoryStats:
    rss_kb: Optional[int] = None
    peak_kb: Optional[int] = None
    hwm_kb: Optional[int] = None
    vm_size_kb: Optional[int] = None
    vm_swap_kb: Optional[int] = None
    threads: Optional[int] = None


class BenchmarkError(RuntimeError):
    pass


# ----------------------------- Runner class -------------------------------- #


class BenchmarkRunner:
    SERVER_PORTS: Dict[str, int] = {
        "aeronet": 8080,
        "drogon": 8081,
        "pistache": 8085,
        "undertow": 8082,
        "go": 8083,
        "python": 8084,
        "rust": 8086,
    }

    SERVER_ORDER = ["aeronet", "drogon", "pistache", "rust", "undertow", "go", "python"]

    SCENARIOS: Dict[str, Scenario] = {
        "headers": Scenario("headers", "lua/headers_stress.lua", "/headers"),
        "body": Scenario("body", "lua/large_body.lua", "/uppercase"),
        "static": Scenario("static", "lua/static_routes.lua", "/ping"),
        "cpu": Scenario("cpu", "lua/cpu_bound.lua", "/compute"),
        "mixed": Scenario("mixed", "lua/mixed_workload.lua", "/"),
        "files": Scenario(
            "files",
            "lua/static_files.lua",
            "/index.html",
            requires_restart=True,
            requires_static=True,
        ),
        "routing": Scenario(
            "routing",
            "lua/routing_stress.lua",
            "/rXXX",
            requires_restart=True,
        ),
        "tls": Scenario(
            "tls",
            "lua/tls_handshake.lua",
            "/ping",
            requires_restart=True,
            requires_tls=True,
            use_https=True,
        ),
    }

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.script_dir = Path(__file__).resolve().parent
        self.logs_dir = self.script_dir / "logs"
        self.logs_dir.mkdir(exist_ok=True)
        self.repo_script_dir = self._detect_repo_script_dir()
        self.build_dir = self._find_build_dir()

        self.threads = max(1, args.threads)
        self.connections = args.connections
        self.duration = args.duration
        self.warmup = args.warmup
        self.output_dir = Path(args.output).resolve()
        self.output_dir.mkdir(parents=True, exist_ok=True)
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        self.result_file = self.output_dir / f"benchmark_{timestamp}.txt"

        self.server_processes: Dict[str, ProcessHandle] = {}
        self.results_rps: Dict[Tuple[str, str], str] = {}
        self.results_latency: Dict[Tuple[str, str], str] = {}
        self.results_transfer: Dict[Tuple[str, str], str] = {}
        self.memory_usage: Dict[Tuple[str, str], MemoryStats] = {}

        self.servers_to_test = self._resolve_server_filter(args.server)
        self.scenarios_to_test = self._resolve_scenario_filter(args.scenario)

        self.needs_static = any(
            self.SCENARIOS[s].requires_static
            for s in self.scenarios_to_test
            if s in self.SCENARIOS
        )
        self.needs_tls = any(
            self.SCENARIOS[s].requires_tls
            for s in self.scenarios_to_test
            if s in self.SCENARIOS
        )

    # ------------------------- Setup helper methods ------------------------- #

    def _detect_repo_script_dir(self) -> Path:
        try:
            git_top = subprocess.run(
                ["git", "rev-parse", "--show-toplevel"],
                capture_output=True,
                text=True,
                check=True,
            ).stdout.strip()
            candidate = Path(git_top) / "benchmarks" / "scripted-servers"
            if candidate.is_dir():
                return candidate
        except Exception:
            pass
        # fallback: assume repo root is two levels up
        return (
            (self.script_dir / ".." / "..").resolve()
            / "benchmarks"
            / "scripted-servers"
        )

    def _find_build_dir(self) -> Path:
        candidates = [
            self.script_dir / "../../build-release/benchmarks/scripted-servers",
            self.script_dir / "../../build/benchmarks/scripted-servers",
            self.script_dir / "../build-release/benchmarks/scripted-servers",
            self.script_dir / "../build/benchmarks/scripted-servers",
            self.script_dir,
        ]
        for cand in candidates:
            cand_path = cand.resolve()
            if cand_path.is_dir():
                return cand_path
        return self.script_dir

    def _resolve_server_filter(self, server_arg: str) -> List[str]:
        if server_arg == "all":
            available = []
            for name in self.SERVER_ORDER:
                if self._server_available(name):
                    available.append(name)
            if not available:
                raise BenchmarkError("No servers available to test")
            return available
        names = [s.strip() for s in server_arg.split(",") if s.strip()]
        for name in names:
            if name not in self.SERVER_PORTS:
                raise BenchmarkError(f"Unknown server: {name}")
            if not self._server_available(name):
                raise BenchmarkError(
                    f"Server '{name}' is not available (missing binary or toolchain)"
                )
        return names

    def _resolve_scenario_filter(self, scenario_arg: str) -> List[str]:
        if scenario_arg == "all":
            return ["headers", "body", "static", "cpu", "mixed", "files", "routing"]
        scenarios = [s.strip() for s in scenario_arg.split(",") if s.strip()]
        for sc in scenarios:
            if sc not in self.SCENARIOS:
                raise BenchmarkError(f"Unknown scenario: {sc}")
        return scenarios

    def _server_available(self, name: str) -> bool:
        try:
            self._prepare_server_command(name, extra_args=None)
            return True
        except BenchmarkError:
            return False

    # --------------------------- Build helpers ----------------------------- #

    def _prepare_server_command(
        self, name: str, extra_args: Optional[Sequence[str]]
    ) -> Tuple[List[str], Optional[Path]]:
        extra_args = list(extra_args or [])
        if name in {"aeronet", "drogon", "pistache"}:
            binary = self.build_dir / f"{name}-bench-server"
            if not binary.is_file():
                raise BenchmarkError(f"Binary not found for {name}: {binary}")
            return [str(binary), *extra_args], None
        if name == "go":
            go_bin = self._ensure_go_server_built()
            return [str(go_bin), *extra_args], go_bin.parent
        if name == "python":
            python_script = self._find_python_server_script()
            return [
                sys.executable or "python3",
                str(python_script),
                *extra_args,
            ], python_script.parent
        if name == "undertow":
            undertow_dir, classpath = self._ensure_undertow_server_built()
            cmd = ["java", "-cp", classpath, "UndertowBenchServer", *extra_args]
            return cmd, undertow_dir
        if name == "rust":
            rust_bin = self._ensure_rust_server_built()
            return [str(rust_bin), *extra_args], rust_bin.parent
        raise BenchmarkError(f"Unsupported server: {name}")

    def _ensure_go_server_built(self) -> Path:
        go_exe = shutil.which("go")
        if not go_exe:
            raise BenchmarkError("Go toolchain not found (go)")
        script_binary = self.script_dir / "go-bench-server"
        if script_binary.is_file():
            return script_binary
        source_candidates = [self.script_dir, self.repo_script_dir]
        for src in source_candidates:
            go_file = src / "go_server.go"
            if go_file.is_file():
                print("Building Go server...")
                subprocess.run(
                    [go_exe, "build", "-o", str(script_binary), str(go_file)],
                    cwd=src,
                    check=True,
                )
                return script_binary
        raise BenchmarkError("go_server.go not found")

    def _ensure_rust_server_built(self) -> Path:
        cargo = shutil.which("cargo")
        if not cargo:
            raise BenchmarkError("Rust toolchain (cargo) not found")
        candidates = [
            self.script_dir / "rust_server",
            self.repo_script_dir / "rust_server",
        ]
        for candidate in candidates:
            if (candidate / "Cargo.toml").is_file():
                print("Building Rust server (release)...")
                subprocess.run([cargo, "build", "--release"], cwd=candidate, check=True)
                binary = candidate / "target" / "release" / "rust-bench-server"
                if binary.is_file():
                    return binary
        raise BenchmarkError("rust_server/Cargo.toml not found")

    def _ensure_undertow_server_built(self) -> Tuple[Path, str]:
        java = shutil.which("java")
        javac = shutil.which("javac")
        if not java or not javac:
            raise BenchmarkError("Java runtime/javac not found")
        undertow_dir = self.script_dir / "undertow_server"
        repo_undertow = self.repo_script_dir / "undertow_server"
        undertow_dir.mkdir(parents=True, exist_ok=True)
        source_file = undertow_dir / "UndertowBenchServer.java"
        if (
            not source_file.is_file()
            and repo_undertow.joinpath("UndertowBenchServer.java").is_file()
        ):
            shutil.copy2(repo_undertow / "UndertowBenchServer.java", source_file)
        if not source_file.is_file():
            raise BenchmarkError("UndertowBenchServer.java not found")
        jars = [
            "undertow-core-2.3.20.Final.jar",
            "xnio-api-3.8.17.Final.jar",
            "xnio-nio-3.8.17.Final.jar",
            "jboss-logging-3.6.1.Final.jar",
            "wildfly-common-2.0.1.jar",
            "jboss-threads-3.9.2.jar",
            "smallrye-common-net-2.14.0.jar",
            "smallrye-common-cpu-2.14.0.jar",
            "smallrye-common-expression-2.14.0.jar",
            "smallrye-common-os-2.14.0.jar",
            "smallrye-common-ref-2.14.0.jar",
            "smallrye-common-constraint-2.14.0.jar",
        ]
        base_url = "https://repo1.maven.org/maven2"
        jar_urls = {
            "undertow-core-2.3.20.Final.jar": f"{base_url}/io/undertow/undertow-core/2.3.20.Final/undertow-core-2.3.20.Final.jar",
            "xnio-api-3.8.17.Final.jar": f"{base_url}/org/jboss/xnio/xnio-api/3.8.17.Final/xnio-api-3.8.17.Final.jar",
            "xnio-nio-3.8.17.Final.jar": f"{base_url}/org/jboss/xnio/xnio-nio/3.8.17.Final/xnio-nio-3.8.17.Final.jar",
            "jboss-logging-3.6.1.Final.jar": f"{base_url}/org/jboss/logging/jboss-logging/3.6.1.Final/jboss-logging-3.6.1.Final.jar",
            "wildfly-common-2.0.1.jar": f"{base_url}/org/wildfly/common/wildfly-common/2.0.1/wildfly-common-2.0.1.jar",
            "jboss-threads-3.9.2.jar": f"{base_url}/org/jboss/threads/jboss-threads/3.9.2/jboss-threads-3.9.2.jar",
            "smallrye-common-net-2.14.0.jar": f"{base_url}/io/smallrye/common/smallrye-common-net/2.14.0/smallrye-common-net-2.14.0.jar",
            "smallrye-common-cpu-2.14.0.jar": f"{base_url}/io/smallrye/common/smallrye-common-cpu/2.14.0/smallrye-common-cpu-2.14.0.jar",
            "smallrye-common-expression-2.14.0.jar": f"{base_url}/io/smallrye/common/smallrye-common-expression/2.14.0/smallrye-common-expression-2.14.0.jar",
            "smallrye-common-os-2.14.0.jar": f"{base_url}/io/smallrye/common/smallrye-common-os/2.14.0/smallrye-common-os-2.14.0.jar",
            "smallrye-common-ref-2.14.0.jar": f"{base_url}/io/smallrye/common/smallrye-common-ref/2.14.0/smallrye-common-ref-2.14.0.jar",
            "smallrye-common-constraint-2.14.0.jar": f"{base_url}/io/smallrye/common/smallrye-common-constraint/2.14.0/smallrye-common-constraint-2.14.0.jar",
        }
        for jar in jars:
            jar_path = undertow_dir / jar
            if not jar_path.is_file():
                url = jar_urls[jar]
                print(f"Downloading {jar}...")
                urllib.request.urlretrieve(url, jar_path)
        classpath = ":".join(["."] + [jar for jar in jars])
        class_files = list(undertow_dir.glob("*.class"))
        if not class_files:
            print("Compiling Undertow benchmark server...")
            subprocess.run(
                [javac, "-cp", classpath, "UndertowBenchServer.java"],
                cwd=undertow_dir,
                check=True,
            )
        return undertow_dir, classpath

    def _find_python_server_script(self) -> Path:
        candidates = [
            self.script_dir / "python_server.py",
            self.repo_script_dir / "python_server.py",
        ]
        additional = [
            self.script_dir / "../../../benchmarks/scripted-servers",
            self.script_dir / "../../benchmarks/scripted-servers",
            self.script_dir / "../benchmarks/scripted-servers",
        ]
        candidates.extend(path.resolve() for path in additional)
        for base in candidates:
            if base.is_file():
                return base
            if base.is_dir():
                candidate = base / "python_server.py"
                if candidate.is_file():
                    return candidate
        raise BenchmarkError("python_server.py not found")

    # --------------------------- Public workflow ---------------------------- #

    def run(self) -> None:
        self._ensure_wrk_available()
        self._write_result_header()
        self._prepare_resources_if_needed()
        print("Starting benchmarks...\n")
        print(f"Results will be saved to: {self.result_file}\n")
        try:
            for server in self.servers_to_test:
                self._run_server_suite(server)
        finally:
            self._stop_all_servers()
        self._print_results_table()
        self._print_memory_table()
        self._write_memory_summary_table()
        self._write_summary_table()
        self._write_json_summary()
        print("\nBenchmarks complete!\n")

    # -------------------------- Resource Preparation ------------------------ #

    def _prepare_resources_if_needed(self) -> None:
        if not (self.needs_static or self.needs_tls):
            return
        script_cmd: Optional[List[str]] = None
        search_roots = [self.script_dir]
        if self.repo_script_dir not in search_roots:
            search_roots.append(self.repo_script_dir)

        for base in search_roots:
            candidate = base / "setup_bench_resources.py"
            if candidate.is_file():
                script_cmd = [sys.executable, str(candidate)]
                break

        if script_cmd is None:
            for base in search_roots:
                candidate = base / "setup_bench_resources.sh"
                if candidate.is_file():
                    script_cmd = ["bash", str(candidate)]
                    break

        if script_cmd is None:
            print(
                "WARNING: setup_bench_resources.{py|sh} not found; static/tls scenarios may fail"
            )
            return

        print("Setting up benchmark resources...")
        subprocess.run([*script_cmd, str(self.script_dir)], check=True)

    # -------------------------- Server lifecycle --------------------------- #

    def _run_server_suite(self, server: str) -> None:
        print("==========================================")
        print(f"Testing: {server}")
        print("==========================================")
        scenarios = [sc for sc in self.scenarios_to_test if sc in self.SCENARIOS]
        normal = [sc for sc in scenarios if not self.SCENARIOS[sc].requires_restart]
        special = [sc for sc in scenarios if self.SCENARIOS[sc].requires_restart]

        if normal:
            if self._start_server(
                server, extra_args=None, scheme="http", insecure=False
            ):
                for scenario in normal:
                    self._run_single(server, scenario)
                self._stop_server(server)
                time.sleep(1)

        for scenario in special:
            scenario_meta = self.SCENARIOS[scenario]
            scheme = "https" if scenario_meta.use_https else "http"
            insecure = scenario_meta.requires_tls
            extra_args = self._scenario_server_args(server, scenario)
            if scenario == "files":
                self._ensure_test_static_files()
            if scenario == "tls" and server != "aeronet":
                print(f"Skipping TLS for {server} (not supported)")
                continue
            print(f"Starting {server} with extra args: {extra_args or ['(none)']}")
            if self._start_server(
                server, extra_args=extra_args, scheme=scheme, insecure=insecure
            ):
                self._run_single(server, scenario)
                self._stop_server(server)
                time.sleep(1)

    def _start_server(
        self,
        server: str,
        *,
        extra_args: Optional[Sequence[str]],
        scheme: str,
        insecure: bool,
    ) -> bool:
        if server in self.server_processes:
            return True
        port = self.SERVER_PORTS[server]
        if self._is_port_in_use(port):
            print(f"ERROR: Port {port} already in use; skipping {server}")
            return False
        try:
            cmd, cwd = self._prepare_server_command(server, extra_args or [])
        except BenchmarkError as exc:
            print(f"Skipping {server}: {exc}")
            return False
        env = os.environ.copy()
        env["BENCH_PORT"] = str(port)
        env["BENCH_THREADS"] = str(self.threads)
        log_path = self.logs_dir / f"{server}.log"
        log_fp = log_path.open("w", encoding="utf-8", errors="replace")
        try:
            popen = subprocess.Popen(
                cmd,
                stdout=log_fp,
                stderr=subprocess.STDOUT,
                cwd=cwd or self.script_dir,
                env=env,
                preexec_fn=os.setsid,
            )
        except Exception as exc:
            log_fp.close()
            print(f"Failed to start {server}: {exc}")
            return False
        self.server_processes[server] = ProcessHandle(
            popen=popen, log_fp=log_fp, log_path=log_path, port=port
        )
        if not self._wait_for_server(port, scheme, insecure):
            print(f"ERROR: {server} failed to report ready; see {log_path}")
            self._stop_server(server)
            return False
        print(f"{server} server ready (PID: {popen.pid})")
        return True

    def _stop_server(self, server: str) -> None:
        handle = self.server_processes.pop(server, None)
        if not handle:
            return
        proc = handle.popen
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait(timeout=1)
        if handle.log_fp:
            handle.log_fp.close()

    def _stop_all_servers(self) -> None:
        for server in list(self.server_processes.keys()):
            self._stop_server(server)

    # ---------------------------- Benchmark logic --------------------------- #

    def _run_single(self, server: str, scenario_name: str) -> None:
        scenario = self.SCENARIOS[scenario_name]
        lua_script = self.script_dir / scenario.lua_script
        if not lua_script.is_file():
            print(f"WARNING: Lua script not found: {lua_script}")
            return
        port = self.SERVER_PORTS[server]
        scheme = "https" if scenario.use_https else "http"
        endpoint = scenario.endpoint
        url = f"{scheme}://127.0.0.1:{port}{endpoint}"
        print(f"\n>>> Running: {server} / {scenario_name}")
        print(f"    Script: {lua_script.relative_to(self.script_dir)}")
        print(f"    URL: {url}")
        warmup_cmd = [
            "wrk",
            f"-t{self.threads}",
            f"-c{self.connections}",
            f"-d{self.warmup}",
            "-s",
            str(lua_script),
            url,
        ]
        subprocess.run(warmup_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        bench_cmd = [
            "wrk",
            "--timeout",
            "30s",
            f"-t{self.threads}",
            f"-c{self.connections}",
            f"-d{self.duration}",
            "-s",
            str(lua_script),
            url,
        ]
        try:
            result = subprocess.run(
                bench_cmd, capture_output=True, text=True, check=True
            )
            output = result.stdout
        except subprocess.CalledProcessError as exc:
            output = (exc.stdout or "") + (exc.stderr or "")
            print(
                f"ERROR: wrk failed for {server} / {scenario_name} (exit {exc.returncode})"
            )
            print(output)
            self._store_result(server, scenario_name, "-", "-", "-")
            self._append_result_block(server, scenario_name, output, error=True)
            self._record_memory_usage(server, scenario_name)
            return
        metrics = self._parse_wrk_output(output)
        self._store_result(
            server,
            scenario_name,
            metrics["rps"],
            metrics["latency"],
            metrics["transfer"],
        )
        print(output)
        self._append_result_block(server, scenario_name, output, error=False)
        self._record_memory_usage(server, scenario_name)

    def _parse_wrk_output(self, output: str) -> Dict[str, str]:
        values = {"rps": "-", "latency": "-", "transfer": "-"}
        non2xx = 0
        for line in output.splitlines():
            line = line.strip()
            if line.startswith("Non-2xx"):
                try:
                    non2xx = int(line.split(":", 1)[1])
                except Exception:
                    non2xx = 1
            elif line.startswith("Requests/sec"):
                values["rps"] = line.split(":", 1)[1].strip()
            elif line.startswith("Latency") and values["latency"] == "-":
                parts = line.split()
                if len(parts) >= 2:
                    values["latency"] = parts[1]
            elif line.startswith("Transfer/sec"):
                values["transfer"] = line.split(":", 1)[1].strip()
        if non2xx:
            print(f"WARNING: wrk reported {non2xx} non-2xx responses; ignoring metrics")
            return {"rps": "-", "latency": "-", "transfer": "-"}
        return values

    def _store_result(
        self, server: str, scenario: str, rps: str, latency: str, transfer: str
    ) -> None:
        key = (server, scenario)
        self.results_rps[key] = rps
        self.results_latency[key] = latency
        self.results_transfer[key] = transfer

    def _append_result_block(
        self, server: str, scenario: str, output: str, error: bool
    ) -> None:
        with self.result_file.open("a", encoding="utf-8") as fp:
            fp.write(f"=== {server} / {scenario}{' (ERROR)' if error else ''} ===\n")
            fp.write(output)
            fp.write("\n\n")

    # ---------------------------- Results output ---------------------------- #

    def _write_result_header(self) -> None:
        sys_info = subprocess.run(
            ["uname", "-a"], capture_output=True, text=True
        ).stdout.strip()
        cpu_info = ""
        cpuinfo = Path("/proc/cpuinfo")
        if cpuinfo.is_file():
            for line in cpuinfo.read_text().splitlines():
                if line.startswith("model name"):
                    cpu_info = line.split(":", 1)[1].strip()
                    break
        with self.result_file.open("w", encoding="utf-8") as fp:
            fp.write("HTTP Server Benchmark Results\n")
            fp.write("==============================\n")
            fp.write(f"Date: {time.ctime()}\n")
            fp.write(f"Threads: {self.threads}\n")
            fp.write(f"Connections: {self.connections}\n")
            fp.write(f"Duration: {self.duration}\n")
            fp.write(f"System: {sys_info}\n")
            if cpu_info:
                fp.write(f"CPU: {cpu_info}\n")
            fp.write("\n")

    def _print_results_table(self) -> None:
        if not self.results_rps:
            return
        print_results = TablePrinter(
            self.servers_to_test,
            self.scenarios_to_test,
            self.results_rps,
            self.results_latency,
            self.results_transfer,
        )
        print_results.print_all()

    def _write_summary_table(self) -> None:
        if not self.results_rps:
            return
        thread_display = str(self.threads)
        with self.result_file.open("a", encoding="utf-8") as fp:
            fp.write("\n=== SUMMARY TABLE ===\n\n")
            header = ["Scenario", "Threads", *self.servers_to_test, "Winner"]
            fp.write(" | ".join(f"{h:<14}" for h in header) + "\n")
            sep = (
                "------------|--------|"
                + "".join("----------------|" for _ in self.servers_to_test)
                + "--------"
            )
            fp.write(sep + "\n")
            for scenario in self.scenarios_to_test:
                row = [f"{scenario:<12}", f"{thread_display:<7}"]
                best_server = self._best_server_for_scenario(scenario)
                for server in self.servers_to_test:
                    val = self.results_rps.get((server, scenario), "-")
                    row.append(f"{format_rps(val):<14}")
                row.append(best_server or "-")
                fp.write(" | ".join(row) + "\n")

    def _write_json_summary(self) -> None:
        """Write a machine-readable JSON summary for CI publishing.

        This is intended to be consumed by downstream tooling (e.g. GitHub Pages
        or badges) without scraping the pretty-printed tables.
        """
        if not self.results_rps:
            return

        summary = {
            "threads": self.threads,
            "duration": self.duration,
            "warmup": self.warmup,
            "servers": self.servers_to_test,
            "scenarios": self.scenarios_to_test,
            "results": {},
        }

        for scenario in self.scenarios_to_test:
            scenario_entry = {"rps": {}, "latency": {}, "transfer": {}, "winners": {}}
            best_server = self._best_server_for_scenario(scenario)
            if best_server is not None:
                scenario_entry["winners"]["rps"] = best_server
            for server in self.servers_to_test:
                key = (server, scenario)
                rps_val = self.results_rps.get(key)
                lat_val = self.results_latency.get(key)
                xfer_val = self.results_transfer.get(key)
                if rps_val is not None:
                    scenario_entry["rps"][server] = rps_val
                if lat_val is not None:
                    scenario_entry["latency"][server] = lat_val
                if xfer_val is not None:
                    scenario_entry["transfer"][server] = xfer_val
                # include memory stats if available
                mem = self.memory_usage.get(key)
                if mem is not None:
                    scenario_entry.setdefault("memory", {})[server] = {
                        "rss_mb": self._kb_to_mb(mem.rss_kb),
                        "peak_mb": self._kb_to_mb(mem.peak_kb),
                        "vmhwm_mb": self._kb_to_mb(mem.hwm_kb),
                        "vmsize_mb": self._kb_to_mb(mem.vm_size_kb),
                        "swap_mb": self._kb_to_mb(mem.vm_swap_kb),
                        "threads": mem.threads,
                    }
            summary["results"][scenario] = scenario_entry

        json_path = self.output_dir / "benchmark_latest.json"
        with json_path.open("w", encoding="utf-8") as jf:
            json.dump(summary, jf, indent=2)

        self._write_badge_summary(summary)

    @staticmethod
    def _kb_to_mb(kb: Optional[int]) -> Optional[float]:
        if kb is None:
            return None
        return round(kb / 1024.0, 3)

    def _write_badge_summary(self, summary: Dict[str, Any]) -> None:
        results = summary.get("results", {})
        best_value = 0.0
        best_scenario = None
        for scenario, data in results.items():
            aeronet_val = data.get("rps", {}).get("aeronet")
            numeric = self._parse_float(aeronet_val)
            if numeric is None:
                continue
            if numeric > best_value:
                best_value = numeric
                best_scenario = scenario

        if best_scenario is None or best_value <= 0:
            return

        badge_payload = {
            "schemaVersion": 1,
            "label": "aeronet peak rps",
            "message": f"{self._format_badge_value(best_value)} req/s",
            "color": self._badge_color(best_value),
            "labelColor": "#0f172a",
            "namedLogo": "speedtest",
            "cacheSeconds": 3600,
        }
        badge_path = self.output_dir / "benchmark_badge.json"
        with badge_path.open("w", encoding="utf-8") as bf:
            json.dump(badge_payload, bf, indent=2)

    @staticmethod
    def _parse_float(value: Optional[str]) -> Optional[float]:
        if value is None:
            return None
        try:
            return float(str(value).replace(",", ""))
        except ValueError:
            return None

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

    def _print_memory_table(self) -> None:
        rows = self._memory_summary_rows()
        if not rows:
            return
        # Boxed memory table similar to other summary boxes (threads removed)
        scenario_w = 12
        server_w = 12
        mem_w = 12
        cols = [
            ("Scenario", scenario_w),
            ("Server", server_w),
            ("RSS", mem_w),
            ("Peak", mem_w),
            ("VMHWM", mem_w),
            ("VMSize", mem_w),
            ("Swap", mem_w),
        ]
        # build header string to compute interior width
        header_cells = [f"{name}".ljust(width) for name, width in cols]
        header_row = " │ ".join(header_cells)
        interior = len(header_row) + 2  # padding inside borders
        border = "═" * interior
        print("╔" + border + "╗")
        title = "MEMORY USAGE SUMMARY"
        subtitle = "(values from /proc/<pid>/status)"
        for text in (title, subtitle):
            left = (interior - len(text)) // 2
            right = interior - len(text) - left
            print(f"║{' ' * left}{text}{' ' * right}║")
        print("╠" + border + "╣")
        print(f"║ {header_row} ║")
        print("╠" + border + "╣")
        for scenario, server, stats in rows:
            cells = [
                f"{scenario:<{scenario_w}}",
                f"{server:<{server_w}}",
                f"{self._format_mem_mb(stats.rss_kb):>{mem_w}}",
                f"{self._format_mem_mb(stats.peak_kb):>{mem_w}}",
                f"{self._format_mem_mb(stats.hwm_kb):>{mem_w}}",
                f"{self._format_mem_mb(stats.vm_size_kb):>{mem_w}}",
                f"{self._format_mem_mb(stats.vm_swap_kb):>{mem_w}}",
            ]
            row = " │ ".join(cells)
            print(f"║ {row} ║")
        print("╚" + border + "╝")

    def _write_memory_summary_table(self) -> None:
        rows = self._memory_summary_rows()
        if not rows:
            return
        with self.result_file.open("a", encoding="utf-8") as fp:
            fp.write("\n=== MEMORY USAGE SUMMARY ===\n\n")
            fp.write(
                "Scenario       | Server       | RSS       | Peak      | VMHWM     | VMSize    | Swap      \n"
            )
            fp.write(
                "--------------|--------------|-----------|-----------|-----------|-----------|-----------\n"
            )
            for scenario, server, stats in rows:
                fp.write(
                    f"{scenario:<14}| {server:<12}| {self._format_mem_mb(stats.rss_kb):>9} | {self._format_mem_mb(stats.peak_kb):>9} | "
                    f"{self._format_mem_mb(stats.hwm_kb):>9} | {self._format_mem_mb(stats.vm_size_kb):>9} | {self._format_mem_mb(stats.vm_swap_kb):>9}\n"
                )

    def _memory_summary_rows(self) -> List[Tuple[str, str, MemoryStats]]:
        rows: List[Tuple[str, str, MemoryStats]] = []
        for scenario in self.scenarios_to_test:
            for server in self.servers_to_test:
                stats = self.memory_usage.get((server, scenario))
                if stats:
                    rows.append((scenario, server, stats))
        return rows

    def _record_memory_usage(self, server: str, scenario: str) -> None:
        handle = self.server_processes.get(server)
        if not handle:
            return
        stats = self._read_memory_stats(handle.popen.pid)
        if stats:
            self.memory_usage[(server, scenario)] = stats

    def _read_memory_stats(self, pid: int) -> Optional[MemoryStats]:
        # Aggregate stats for the process and any child/worker processes it may have
        # spawned (e.g., Python servers using worker processes). We scan /proc to
        # find descendant PIDs (by PPid) and sum relevant Vm* values. This gives a
        # more realistic memory footprint for multi-process servers.
        proc_root = Path(f"/proc/{pid}/status")
        if not proc_root.is_file():
            return None

        # Build parent map: pid -> ppid for all numeric /proc entries
        ppid_map = {}
        for entry in Path("/proc").iterdir():
            if not entry.name.isdigit():
                continue
            try:
                text = entry.joinpath("status").read_text()
            except Exception:
                continue
            # quick parse for Pid and PPid
            p = None
            pp = None
            for line in text.splitlines():
                if line.startswith("Pid:"):
                    p = int(line.split(":", 1)[1].strip())
                elif line.startswith("PPid:"):
                    pp = int(line.split(":", 1)[1].strip())
                if p is not None and pp is not None:
                    break
            if p is not None and pp is not None:
                ppid_map[p] = pp

        # collect descendants of pid (including pid)
        to_visit = [pid]
        descendants = set()
        while to_visit:
            cur = to_visit.pop()
            if cur in descendants:
                continue
            descendants.add(cur)
            for child_pid, parent_pid in ppid_map.items():
                if parent_pid == cur and child_pid not in descendants:
                    to_visit.append(child_pid)

        agg = MemoryStats()
        # For each discovered PID, read its /proc/<pid>/status and sum numeric fields
        for p in sorted(descendants):
            status_path = Path(f"/proc/{p}/status")
            if not status_path.is_file():
                continue
            for line in status_path.read_text().splitlines():
                key, _, value = line.partition(":")
                token = value.strip()
                if key == "VmRSS":
                    v = self._parse_kb(token)
                    if v is not None:
                        agg.rss_kb = (agg.rss_kb or 0) + v
                elif key == "VmPeak":
                    v = self._parse_kb(token)
                    if v is not None:
                        agg.peak_kb = (agg.peak_kb or 0) + v
                elif key == "VmHWM":
                    v = self._parse_kb(token)
                    if v is not None:
                        agg.hwm_kb = (agg.hwm_kb or 0) + v
                elif key == "VmSize":
                    v = self._parse_kb(token)
                    if v is not None:
                        agg.vm_size_kb = (agg.vm_size_kb or 0) + v
                elif key == "VmSwap":
                    v = self._parse_kb(token)
                    if v is not None:
                        agg.vm_swap_kb = (agg.vm_swap_kb or 0) + v
                elif key == "Threads":
                    t = self._parse_int(token)
                    if t is not None:
                        agg.threads = (agg.threads or 0) + t
        return agg

    @staticmethod
    def _parse_kb(value: str) -> Optional[int]:
        if not value:
            return None
        parts = value.split()
        if not parts:
            return None
        try:
            return int(parts[0])
        except ValueError:
            return None

    @staticmethod
    def _parse_int(value: str) -> Optional[int]:
        if not value:
            return None
        try:
            return int(value.split()[0])
        except ValueError:
            return None

    @staticmethod
    def _format_mem_mb(kb: Optional[int]) -> str:
        if kb is None:
            return "-"
        return f"{kb / 1024:.1f}MB"

    @staticmethod
    def _format_count(value: Optional[int]) -> str:
        return str(value) if value is not None else "-"

    def _best_server_for_scenario(self, scenario: str) -> str:
        best_name = ""
        best_val = 0
        for server in self.servers_to_test:
            val = self.results_rps.get((server, scenario))
            if val and val != "-":
                try:
                    numeric = float(val)
                except ValueError:
                    continue
                if numeric > best_val:
                    best_val = numeric
                    best_name = server
        return best_name

    # ----------------------------- Utilities -------------------------------- #

    def _scenario_server_args(self, server: str, scenario: str) -> List[str]:
        args: List[str] = []
        meta = self.SCENARIOS[scenario]
        static_dir = self.script_dir / "static"
        certs_dir = self.script_dir / "certs"
        if meta.requires_static and static_dir.is_dir():
            args += ["--static", str(static_dir)]
        if scenario == "routing":
            args += ["--routes", "1000"]
        if scenario == "tls" and server == "aeronet" and certs_dir.is_dir():
            cert = certs_dir / "server.crt"
            key = certs_dir / "server.key"
            if cert.is_file() and key.is_file():
                args += ["--tls", "--cert", str(cert), "--key", str(key)]
        return args

    def _ensure_test_static_files(self) -> None:
        static_dir = self.script_dir / "static"
        static_dir.mkdir(exist_ok=True)
        file_names_and_sizes = [
            ("large.bin", 1 * 1024 * 1024 * 1024),
            ("medium.bin", 10 * 1024 * 1024),
        ]
        for file_name, size_bytes in file_names_and_sizes:
            target = static_dir / file_name
            if target.is_file():
                continue
            print(
                f"Creating static test file: {file_name} ({size_bytes // (1024 * 1024)} MB)"
            )
            fallocate = shutil.which("fallocate")
            if fallocate:
                subprocess.run(
                    [fallocate, "-l", str(size_bytes), str(target)], check=True
                )
            else:
                with target.open("wb") as fp:
                    fp.seek(size_bytes - 1)
                    fp.write(b"\0")

    def _ensure_wrk_available(self) -> None:
        if not shutil.which("wrk"):
            raise BenchmarkError("wrk not found in PATH")

    def _is_port_in_use(self, port: int) -> bool:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.settimeout(0.1)
            return sock.connect_ex(("127.0.0.1", port)) == 0

    def _wait_for_server(self, port: int, scheme: str, insecure: bool) -> bool:
        url = f"{scheme}://127.0.0.1:{port}/status"
        context = ssl.create_default_context()
        if insecure:
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
        for _ in range(50):
            try:
                req = urllib.request.Request(url)
                with urllib.request.urlopen(
                    req, timeout=0.5, context=context if scheme == "https" else None
                ):
                    return True
            except Exception:
                time.sleep(0.2)
        return False


# ------------------------------ Table printer ------------------------------ #


def format_rps(value: Optional[str]) -> str:
    if not value or value == "-":
        return "-"
    try:
        return f"{int(float(value)):,}"
    except ValueError:
        return value


class TablePrinter:
    def __init__(
        self,
        servers: List[str],
        scenarios: List[str],
        rps: Dict[Tuple[str, str], str],
        latency: Dict[Tuple[str, str], str],
        transfer: Dict[Tuple[str, str], str],
    ) -> None:
        self.servers = servers
        self.scenarios = scenarios
        self.rps = rps
        self.latency = latency
        self.transfer = transfer

    def print_all(self) -> None:
        self._print_box(
            "BENCHMARK RESULTS COMPARISON",
            "(Requests/sec - higher is better)",
            self.rps,
            higher_is_better=True,
        )
        self._print_box(
            "LATENCY COMPARISON",
            "(Average - lower is better)",
            self.latency,
            higher_is_better=False,
        )
        self._print_box(
            "TRANSFER RATE COMPARISON",
            "(Data throughput - higher is better)",
            self.transfer,
            higher_is_better=True,
        )

    def _print_box(
        self,
        title: str,
        subtitle: str,
        data: Dict[Tuple[str, str], str],
        higher_is_better: bool,
    ) -> None:
        scenario_width = 12
        cell_width = 14
        win_width = 10
        interior = (
            scenario_width + 3 + len(self.servers) * (cell_width + 3) + win_width + 2
        )
        border = "═" * interior
        print("╔" + border + "╗")
        for text in (title, subtitle):
            left = (interior - len(text)) // 2
            right = interior - len(text) - left
            print(f"║{' ' * left}{text}{' ' * right}║")
        print("╠" + border + "╣")
        header = [f"║ {'Scenario':<{scenario_width}} │"]
        for srv in self.servers:
            header.append(f" {srv:<{cell_width}} │")
        label = "Winner" if higher_is_better else "Best"
        header.append(f" {label:<{win_width}} ║")
        print("".join(header))
        print("╠" + border + "╣")
        for scenario in self.scenarios:
            row = [f"║ {scenario:<{scenario_width}} │"]
            best_server = self._best_server(scenario, data, higher_is_better)
            for srv in self.servers:
                val = data.get((srv, scenario), "-")
                display = val
                cell = f" {display:<{cell_width}} │"
                if srv == best_server and display != "-":
                    truncated = display[: cell_width - 2]
                    cell = f" {truncated:<{cell_width - 2}} \033[1;32m★\033[0m │"
                row.append(cell)
            row.append(f" {best_server or '-':<{win_width}} ║")
            print("".join(row))
        print("╚" + border + "╝\n")

    def _best_server(
        self, scenario: str, data: Dict[Tuple[str, str], str], higher_is_better: bool
    ) -> str:
        cmp_value = None
        best_name = ""
        for srv in self.servers:
            val = data.get((srv, scenario))
            if not val or val == "-":
                continue
            numeric = self._to_numeric(val, higher_is_better)
            if numeric is None:
                continue
            if cmp_value is None or (
                numeric > cmp_value if higher_is_better else numeric < cmp_value
            ):
                cmp_value = numeric
                best_name = srv
        return best_name

    @staticmethod
    def _to_numeric(value: str, higher_is_better: bool) -> Optional[float]:
        try:
            if any(value.endswith(unit) for unit in ("us", "ms", "s")):
                suffix = "".join(ch for ch in value if not ch.isdigit() and ch != ".")
                number = float("".join(ch for ch in value if ch.isdigit() or ch == "."))
                scale = {"us": 1, "ms": 1000, "s": 1_000_000}.get(suffix, 1)
                microseconds = number * scale
                return microseconds
            if any(value.endswith(unit) for unit in ("KB", "MB", "GB")):
                suffix = "".join(ch for ch in value if not ch.isdigit() and ch != ".")
                number = float("".join(ch for ch in value if ch.isdigit() or ch == "."))
                scale = {"B": 1, "KB": 1024, "MB": 1_048_576, "GB": 1_073_741_824}.get(
                    suffix, 1
                )
                bytes_value = number * scale
                return bytes_value
            return float(value)
        except ValueError:
            return None


# ------------------------------- CLI parsing ------------------------------- #


def parse_args() -> argparse.Namespace:
    cpu_count = os.cpu_count() or 1
    default_threads = int(os.environ.get("BENCH_THREADS", max(1, cpu_count // 4)))
    default_connections = int(os.environ.get("BENCH_CONNECTIONS", 100))
    default_duration = os.environ.get("BENCH_DURATION", "30s")
    default_warmup = os.environ.get("BENCH_WARMUP", "5s")
    default_output = os.environ.get("BENCH_OUTPUT", "./results")
    parser = argparse.ArgumentParser(
        description="Run wrk benchmarks across multiple servers"
    )
    parser.add_argument(
        "--threads", type=int, default=default_threads, help="Number of wrk threads"
    )
    parser.add_argument(
        "--connections",
        type=int,
        default=default_connections,
        help="Number of wrk connections",
    )
    parser.add_argument(
        "--duration",
        type=str,
        default=default_duration,
        help="Duration per benchmark (e.g. 30s)",
    )
    parser.add_argument(
        "--warmup", type=str, default=default_warmup, help="Warmup duration (e.g. 5s)"
    )
    parser.add_argument(
        "--output",
        type=str,
        default=default_output,
        help="Output directory for results",
    )
    parser.add_argument(
        "--server",
        type=str,
        default="all",
        help="Comma-separated list of servers (aeronet,drogon,pistache,undertow,go,python,rust)",
    )
    parser.add_argument(
        "--scenario",
        type=str,
        default="all",
        help="Comma-separated list of scenarios (headers,body,static,cpu,mixed,files,routing,tls)",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    runner = BenchmarkRunner(args)
    try:
        runner.run()
    except BenchmarkError as exc:
        print(f"ERROR: {exc}")
        sys.exit(1)


if __name__ == "__main__":
    main()
