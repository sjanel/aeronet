#!/usr/bin/env python3
"""Benchmark orchestration script for wrk-based HTTP server comparisons."""
from __future__ import annotations

import argparse
import json
import os
import re
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
from typing import Any, Dict, List, Optional, Sequence, Tuple

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


@dataclass(frozen=True)
class H2Scenario:
    """h2load-specific scenario configuration."""
    name: str
    endpoint: str
    method: str = "GET"
    body_file: Optional[str] = None       # File path for POST data (-d)
    extra_headers: Sequence[str] = ()      # Additional -H flags
    requires_restart: bool = False
    requires_static: bool = False
    connections: Optional[int] = None      # Override global -c for this scenario
    streams: Optional[int] = None          # Override global -m for this scenario


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
        "crow": 8087,
    }

    SERVER_ORDER = ["aeronet", "drogon", "pistache", "crow", "rust", "undertow", "go", "python"]

    # Servers that support HTTP/2 benchmarks (pistache, crow & drogon lack H2 server support)
    H2_SERVER_ORDER = ["aeronet", "rust", "undertow", "go", "python"]

    # Servers that only support H2 over TLS (not h2c cleartext)
    H2_TLS_ONLY_SERVERS: set = set()

    SCENARIOS: Dict[str, Scenario] = {
        "headers": Scenario("headers", "lua/headers_stress.lua", "/headers"),
        "body": Scenario("body", "lua/large_body.lua", "/uppercase"),
        "body-codec": Scenario("body-codec", "lua/body_codec.lua", "/body-codec"),
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

    # H2 scenario definitions for h2load benchmarks.
    # Maps the same scenario names to h2load-friendly parameters.
    H2_SCENARIOS: Dict[str, H2Scenario] = {
        "headers": H2Scenario("headers", "/headers?count=10&size=64"),
        "body": H2Scenario(
            "body",
            "/uppercase",
            method="POST",
            body_file="h2_body_1k.bin",
        ),
        "body-codec": H2Scenario(
            "body-codec",
            "/body-codec",
            method="POST",
            body_file="h2_body_1k.gz",
            extra_headers=("Content-Encoding: gzip", "Accept-Encoding: gzip"),
        ),
        "static": H2Scenario("static", "/ping"),
        "cpu": H2Scenario("cpu", "/compute?complexity=30&hash_iters=1000"),
        "mixed": H2Scenario("mixed", "/ping"),  # multi-URI below
        "files": H2Scenario(
            "files", "/large.bin", requires_restart=True, requires_static=True,
            connections=20, streams=1,  # 25MB per file; limit concurrency to avoid OOM/epoll crashes
        ),
        "routing": H2Scenario("routing", "/r500", requires_restart=True),
    }

    # URIs for the 'mixed' h2load scenario - distributed round-robin
    H2_MIXED_ENDPOINTS = [
        "/ping",
        "/headers?count=5&size=32",
        "/body?size=512",
        "/compute?complexity=20&hash_iters=500",
        "/json?items=5",
    ]

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
        self.wrk_timeout = args.wrk_timeout
        self.wrk_timeout_seconds = self._duration_to_seconds(self.wrk_timeout)
        if self.wrk_timeout_seconds is None:
            raise BenchmarkError(f"Invalid wrk timeout value: {self.wrk_timeout}")

        # HTTP/2 benchmark settings
        self.protocol: str = getattr(args, "protocol", "http1")
        self.h2_streams: int = getattr(args, "h2_streams", 10)

        self.output_dir = Path(args.output).resolve()
        self.output_dir.mkdir(parents=True, exist_ok=True)
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        self.result_file = self.output_dir / f"benchmark_{timestamp}.txt"

        self.server_processes: Dict[str, ProcessHandle] = {}
        self.results_rps: Dict[Tuple[str, str], str] = {}
        self.results_rps_raw: Dict[Tuple[str, str], str] = {}
        self.results_latency: Dict[Tuple[str, str], str] = {}
        self.results_latency_raw: Dict[Tuple[str, str], str] = {}
        self.results_transfer: Dict[Tuple[str, str], str] = {}
        self.results_timeouts: Dict[Tuple[str, str], int] = {}
        self.memory_usage: Dict[Tuple[str, str], MemoryStats] = {}

        self.servers_to_test = self._resolve_server_filter(args.server)
        self.scenarios_to_test = self._resolve_scenario_filter(args.scenario)

        # Track whether any Aeronet scenario reported wrk errors
        self._aeronet_errors_found: bool = False

        self.needs_static = any(
            self.SCENARIOS.get(s, Scenario(s, "", "")).requires_static
            or self.H2_SCENARIOS.get(s, H2Scenario(s, "")).requires_static
            for s in self.scenarios_to_test
        )
        self.needs_tls = any(
            self.SCENARIOS.get(s, Scenario(s, "", "")).requires_tls
            for s in self.scenarios_to_test
        ) or self.protocol == "h2-tls"

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
        build_dir_env = os.environ.get("AERONET_BUILD_DIR")
        if build_dir_env:
            env_path = Path(build_dir_env).resolve()
            if env_path.is_dir():
                return env_path
        
        candidates = [
            self.script_dir / "../../build-pages/benchmarks/scripted-servers",
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
        is_h2 = self.protocol in ("h2c", "h2-tls")
        order = self.H2_SERVER_ORDER if is_h2 else self.SERVER_ORDER
        if is_h2:
            # Show which servers are excluded from H2 benchmarks
            all_h1 = set(self.SERVER_ORDER) - set(self.H2_SERVER_ORDER)
            if all_h1:
                print(f"Note: {', '.join(sorted(all_h1))} excluded from H2 benchmarks (no HTTP/2 server support)")
        if server_arg.startswith("all"):
            available = []
            for name in order:
                if name == "python" and server_arg.endswith("-except-python"):
                    continue
                if is_h2 and self.protocol == "h2c" and name in self.H2_TLS_ONLY_SERVERS:
                    print(f"Skipping {name} for h2c (TLS-only H2 support)")
                    continue
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
        is_h2 = self.protocol in ("h2c", "h2-tls")
        if scenario_arg == "all":
            if is_h2:
                # For H2, skip 'tls' (inherent in h2-tls) and only include H2-mapped scenarios
                return [
                    s for s in [
                        "headers", "body", "body-codec", "static", "cpu", "mixed",
                        "files", "routing",
                    ] if s in self.H2_SCENARIOS
                ]
            return [
                "headers",
                "body",
                "body-codec",
                "static",
                "cpu",
                "mixed",
                "files",
                "routing",
            ]
        scenarios = [s.strip() for s in scenario_arg.split(",") if s.strip()]
        for sc in scenarios:
            if sc not in self.SCENARIOS:
                raise BenchmarkError(f"Unknown scenario: {sc}")
        return scenarios

    def _server_available(self, name: str) -> bool:
        try:
            self._prepare_server_command(name, extra_args=None)
            return True
        except BenchmarkError as exc:
            print(f"Server '{name}' is not available: {exc}")
            return False

    # --------------------------- Build helpers ----------------------------- #

    def _prepare_server_command(
        self, name: str, extra_args: Optional[Sequence[str]]
    ) -> Tuple[List[str], Optional[Path]]:
        extra_args = list(extra_args or [])
        if name in {"aeronet", "drogon", "pistache", "crow"}:
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
        source_candidates = [self.script_dir, self.repo_script_dir]
        go_file = None
        for src in source_candidates:
            candidate = src / "go_server.go"
            if candidate.is_file():
                go_file = candidate
                break
        if go_file is None:
            raise BenchmarkError("go_server.go not found")
        if (not script_binary.is_file()) or (go_file.stat().st_mtime > script_binary.stat().st_mtime):
            print("Building Go server...")
            try:
                subprocess.run(
                    [go_exe, "build", "-o", str(script_binary), str(go_file)],
                    cwd=go_file.parent,
                    check=True,
                )
            except subprocess.CalledProcessError as exc:
                raise BenchmarkError(
                    f"Go server build failed (exit {exc.returncode}); "
                    f"check go modules (try: go mod download)"
                ) from exc
        return script_binary

    def _ensure_rust_server_built(self) -> Path:
        # Prefer rustup cargo over system cargo (handles newer lockfile formats)
        rustup_cargo = Path.home() / ".cargo" / "bin" / "cargo"
        cargo = str(rustup_cargo) if rustup_cargo.is_file() else shutil.which("cargo")
        if not cargo:
            raise BenchmarkError("Rust toolchain (cargo) not found")
        candidates = [
            self.script_dir / "rust_server",
            self.repo_script_dir / "rust_server",
        ]
        for candidate in candidates:
            if (candidate / "Cargo.toml").is_file():
                print("Building Rust server (release)...")
                env = os.environ.copy()
                # Ensure rustup bin dir is in PATH for rustc/rustup detection
                rustup_bin = str(Path.home() / ".cargo" / "bin")
                if rustup_bin not in env.get("PATH", ""):
                    env["PATH"] = rustup_bin + ":" + env.get("PATH", "")
                try:
                    subprocess.run(
                        [cargo, "build", "--release"],
                        cwd=candidate,
                        env=env,
                        check=True,
                    )
                except subprocess.CalledProcessError as exc:
                    raise BenchmarkError(
                        f"Rust server build failed (exit {exc.returncode}); "
                        f"check your rustc version (need >= 1.82)"
                    ) from exc
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
        repo_source = repo_undertow / "UndertowBenchServer.java"
        if repo_source.is_file():
            if (not source_file.is_file()) or (repo_source.stat().st_mtime > source_file.stat().st_mtime):
                shutil.copy2(repo_source, source_file)
        if not source_file.is_file():
            raise BenchmarkError("UndertowBenchServer.java not found")
        jars = [
            "undertow-core-2.3.23.Final.jar",
            "xnio-api-3.8.17.Final.jar",
            "xnio-nio-3.8.17.Final.jar",
            "jboss-logging-3.6.2.Final.jar",
            "wildfly-common-2.0.1.jar",
            "jboss-threads-3.9.2.jar",
            "smallrye-common-net-2.16.0.jar",
            "smallrye-common-cpu-2.16.0.jar",
            "smallrye-common-expression-2.16.0.jar",
            "smallrye-common-os-2.16.0.jar",
            "smallrye-common-ref-2.16.0.jar",
            "smallrye-common-constraint-2.16.0.jar",
        ]
        base_url = "https://repo1.maven.org/maven2"
        jar_urls = {
            "undertow-core-2.3.23.Final.jar": f"{base_url}/io/undertow/undertow-core/2.3.23.Final/undertow-core-2.3.23.Final.jar",
            "xnio-api-3.8.17.Final.jar": f"{base_url}/org/jboss/xnio/xnio-api/3.8.17.Final/xnio-api-3.8.17.Final.jar",
            "xnio-nio-3.8.17.Final.jar": f"{base_url}/org/jboss/xnio/xnio-nio/3.8.17.Final/xnio-nio-3.8.17.Final.jar",
            "jboss-logging-3.6.2.Final.jar": f"{base_url}/org/jboss/logging/jboss-logging/3.6.2.Final/jboss-logging-3.6.2.Final.jar",
            "wildfly-common-2.0.1.jar": f"{base_url}/org/wildfly/common/wildfly-common/2.0.1/wildfly-common-2.0.1.jar",
            "jboss-threads-3.9.2.jar": f"{base_url}/org/jboss/threads/jboss-threads/3.9.2/jboss-threads-3.9.2.jar",
            "smallrye-common-net-2.16.0.jar": f"{base_url}/io/smallrye/common/smallrye-common-net/2.16.0/smallrye-common-net-2.16.0.jar",
            "smallrye-common-cpu-2.16.0.jar": f"{base_url}/io/smallrye/common/smallrye-common-cpu/2.16.0/smallrye-common-cpu-2.16.0.jar",
            "smallrye-common-expression-2.16.0.jar": f"{base_url}/io/smallrye/common/smallrye-common-expression/2.16.0/smallrye-common-expression-2.16.0.jar",
            "smallrye-common-os-2.16.0.jar": f"{base_url}/io/smallrye/common/smallrye-common-os/2.16.0/smallrye-common-os-2.16.0.jar",
            "smallrye-common-ref-2.16.0.jar": f"{base_url}/io/smallrye/common/smallrye-common-ref/2.16.0/smallrye-common-ref-2.16.0.jar",
            "smallrye-common-constraint-2.16.0.jar": f"{base_url}/io/smallrye/common/smallrye-common-constraint/2.16.0/smallrye-common-constraint-2.16.0.jar",
        }
        for jar in jars:
            jar_path = undertow_dir / jar
            if not jar_path.is_file():
                url = jar_urls[jar]
                print(f"Downloading {jar}...")
                urllib.request.urlretrieve(url, jar_path)
        classpath = ":".join(["."] + [jar for jar in jars])
        class_files = list(undertow_dir.glob("*.class"))
        needs_recompile = not class_files
        if not needs_recompile:
            source_mtime = source_file.stat().st_mtime
            needs_recompile = any(source_mtime > class_file.stat().st_mtime for class_file in class_files)
        if needs_recompile:
            print("Compiling Undertow benchmark server...")
            try:
                subprocess.run(
                    [javac, "-cp", classpath, "UndertowBenchServer.java"],
                    cwd=undertow_dir,
                    check=True,
                )
            except subprocess.CalledProcessError as exc:
                raise BenchmarkError(
                    f"Undertow server compilation failed (exit {exc.returncode})"
                ) from exc
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
        is_h2 = self.protocol in ("h2c", "h2-tls")
        if is_h2:
            self._ensure_h2load_available()
            self._prepare_h2load_body_files()
        else:
            self._ensure_wrk_available()
        self._write_result_header()
        self._prepare_resources_if_needed()
        tool = "h2load" if is_h2 else "wrk"
        print(f"Starting benchmarks (protocol={self.protocol}, tool={tool})...\n")
        print(f"Results will be saved to: {self.result_file}\n")
        try:
            for server in self.servers_to_test:
                if is_h2:
                    self._run_server_suite_h2(server)
                else:
                    self._run_server_suite(server)
        finally:
            self._stop_all_servers()
        self._print_results_table()
        self._print_memory_table()
        self._write_memory_summary_table()
        self._write_summary_table()
        self._write_json_summary()
        # Fail the CI if Aeronet had any wrk-reported errors
        if self._aeronet_errors_found:
            print("\nBenchmarks complete (Aeronet reported errors)\n")
            # Non-zero exit code to fail CI
            sys.exit(1)
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
                # First pass: warmup all normal scenarios to reuse the same warmed server state.
                for scenario in normal:
                    self._run_single(server, scenario, warmup=True, warmup_only=True)
                # Second pass: run the real measurements without rerunning warmup.
                for scenario in normal:
                    self._run_single(server, scenario, warmup=False)
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

        # Optional server-specific profiler/runtime environment passthrough.
        # This allows profiling wrappers to instrument only the server process
        # (e.g. aeronet) without affecting wrk/python orchestrator processes.
        server_key = server.upper()
        profiler_vars = (
            "LD_PRELOAD",
            "HEAPPROFILE",
            "HEAP_PROFILE_ALLOCATION_INTERVAL",
            "HEAPPROFILESIGNAL",
            "CPUPROFILE",
            "CPUPROFILE_FREQUENCY",
        )
        for var_name in profiler_vars:
            if var_name in env:
                env.pop(var_name, None)
            scoped_key = f"BENCH_{server_key}_{var_name}"
            scoped_val = os.environ.get(scoped_key)
            if scoped_val:
                env[var_name] = scoped_val

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
            except (ProcessLookupError, OSError):
                pass
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except (ProcessLookupError, OSError):
                    pass
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    # Last resort: kill the process directly (not group)
                    try:
                        proc.kill()
                        proc.wait(timeout=3)
                    except Exception:
                        print(f"WARNING: Could not stop {server} (PID {proc.pid}); process may be orphaned")
        if handle.log_fp:
            handle.log_fp.close()

    def _stop_all_servers(self) -> None:
        for server in list(self.server_processes.keys()):
            self._stop_server(server)

    # ---------------------------- Benchmark logic --------------------------- #

    def _run_single(
        self,
        server: str,
        scenario_name: str,
        *,
        warmup: bool = True,
        warmup_only: bool = False,
    ) -> None:
        scenario = self.SCENARIOS[scenario_name]
        lua_script = self.script_dir / scenario.lua_script
        if not lua_script.is_file():
            print(f"WARNING: Lua script not found: {lua_script}")
            return
        port = self.SERVER_PORTS[server]
        scheme = "https" if scenario.use_https else "http"
        endpoint = scenario.endpoint
        url = f"{scheme}://127.0.0.1:{port}{endpoint}"
        if warmup:
            print(f">>> Warm-up: {server} / {scenario_name}")
            warmup_cmd = [
                "wrk",
                f"-t{self.threads}",
                f"-c{self.connections}",
                f"-d{self.warmup}",
                f"--timeout={self.wrk_timeout}",
                "-s",
                str(lua_script),
                url,
            ]
            subprocess.run(
                warmup_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
            if warmup_only:
                return
        print(f">>> Running: {server} / {scenario_name}")
        print(f"    Script: {lua_script.relative_to(self.script_dir)}")
        print(f"    URL: {url}")
        bench_cmd = [
            "wrk",
            f"-t{self.threads}",
            f"-c{self.connections}",
            f"-d{self.duration}",
            f"--timeout={self.wrk_timeout}",
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
            self._store_result(server, scenario_name, "-", "-", "-", latency_raw="-", timeout_errors=0)
            self._append_result_block(server, scenario_name, output, error=True)
            self._record_memory_usage(server, scenario_name)
            return
        metrics = self._parse_wrk_output(output)
        # Extract wrk error counters printed by lua scripts
        err_connect, err_read, err_write, err_timeout = self._extract_wrk_errors(output)
        non2xx = int(metrics.get("non2xx") or 0)
        any_errs = (
            (err_connect > 0)
            or (err_read > 0)
            or (err_write > 0)
            or (err_timeout > 0)
            or (non2xx > 0)
        )
        if server == "aeronet" and any_errs:
            self._aeronet_errors_found = True
            print(
                f"ERROR: Aeronet reported issues (connect/read/write/timeout/non2xx) = "
                f"{err_connect}/{err_read}/{err_write}/{err_timeout}/{non2xx} for scenario '{scenario_name}'"
            )

        success_requests = int(metrics.get("total_requests") or 0)
        total_errors = err_connect + err_read + err_write + err_timeout + non2xx
        successful_rps = self._compute_success_rps(
            raw_rps=str(metrics.get("rps", "-")),
            total_requests=success_requests,
            total_errors=total_errors,
            measured_duration_seconds=metrics.get("duration_seconds"),
        )
        if successful_rps != metrics["rps"] and successful_rps != "-":
            print(
                "    Adjusted success RPS "
                f"(errors + non-2xx removed): {successful_rps} (raw: {metrics['rps']})"
            )

        adjusted_latency = self._compute_timeout_adjusted_latency(
            metrics["latency"], success_requests, err_timeout, self.wrk_timeout_seconds
        )
        if adjusted_latency != metrics["latency"] and metrics["latency"] != "-":
            print(
                "    Adjusted latency (timeouts counted at wrk timeout "
                f"{self.wrk_timeout}): {adjusted_latency} (raw: {metrics['latency']})"
            )

        self._store_result(
            server,
            scenario_name,
            successful_rps,
            adjusted_latency,
            metrics["transfer"],
            rps_raw=metrics["rps"],
            latency_raw=metrics["latency"],
            timeout_errors=err_timeout,
        )
        print(output)
        self._append_result_block(
            server, scenario_name, output, error=(server == "aeronet" and any_errs)
        )
        self._record_memory_usage(server, scenario_name)

    # ---------------------- HTTP/2 h2load benchmark logic -------------------- #

    def _ensure_h2load_available(self) -> None:
        if not shutil.which("h2load"):
            raise BenchmarkError(
                "h2load not found in PATH. Install nghttp2-client: "
                "apt install nghttp2-client / brew install nghttp2"
            )

    def _prepare_h2load_body_files(self) -> None:
        """Create POST body files used by h2load scenarios."""
        data_dir = self.script_dir / "h2_data"
        data_dir.mkdir(exist_ok=True)
        # 1KB binary body for /uppercase
        body_1k = data_dir / "h2_body_1k.bin"
        if not body_1k.is_file():
            body_1k.write_bytes(os.urandom(1024))
        # 1KB gzipped body for /body-codec
        body_gz = data_dir / "h2_body_1k.gz"
        if not body_gz.is_file():
            import gzip as gzip_mod
            body_gz.write_bytes(gzip_mod.compress(os.urandom(1024)))

    def _run_server_suite_h2(self, server: str) -> None:
        """Run all H2 scenarios for a single server using h2load."""
        print("==========================================")
        print(f"Testing: {server} [{self.protocol}]")
        print("==========================================")
        scenarios = [sc for sc in self.scenarios_to_test if sc in self.H2_SCENARIOS]
        normal = [sc for sc in scenarios if not self.H2_SCENARIOS[sc].requires_restart]
        special = [sc for sc in scenarios if self.H2_SCENARIOS[sc].requires_restart]

        use_tls = self.protocol == "h2-tls"
        scheme = "https" if use_tls else "http"
        h2_args = ["--h2"]
        if use_tls:
            h2_args.append("--tls")
            certs_dir = self.script_dir / "certs"
            if certs_dir.is_dir():
                cert = certs_dir / "server.crt"
                key = certs_dir / "server.key"
                if cert.is_file() and key.is_file():
                    h2_args += ["--cert", str(cert), "--key", str(key)]

        if normal:
            if self._start_server(
                server, extra_args=h2_args, scheme=scheme, insecure=use_tls
            ):
                # Warmup all normal scenarios
                for scenario in normal:
                    self._run_single_h2load(server, scenario, warmup_only=True)
                # Real measurements
                for scenario in normal:
                    self._run_single_h2load(server, scenario)
                self._stop_server(server)
                time.sleep(1)

        for scenario in special:
            h2_meta = self.H2_SCENARIOS[scenario]
            extra = list(h2_args)
            extra += self._h2_scenario_server_args(server, scenario)
            if h2_meta.requires_static:
                self._ensure_test_static_files()
            print(f"Starting {server} with extra args: {extra or ['(none)']}")
            if self._start_server(
                server, extra_args=extra, scheme=scheme, insecure=use_tls
            ):
                self._run_single_h2load(server, scenario)
                self._stop_server(server)
                time.sleep(1)

    def _run_single_h2load(
        self,
        server: str,
        scenario_name: str,
        *,
        warmup_only: bool = False,
    ) -> None:
        """Run a single scenario benchmark using h2load."""
        h2_scenario = self.H2_SCENARIOS.get(scenario_name)
        if h2_scenario is None:
            print(f"WARNING: No H2 scenario mapping for '{scenario_name}'")
            return

        port = self.SERVER_PORTS[server]
        use_tls = self.protocol == "h2-tls"
        scheme = "https" if use_tls else "http"
        base_url = f"{scheme}://127.0.0.1:{port}"

        # Build URL list
        if scenario_name == "mixed":
            urls = [f"{base_url}{ep}" for ep in self.H2_MIXED_ENDPOINTS]
        else:
            urls = [f"{base_url}{h2_scenario.endpoint}"]

        duration_seconds = self._duration_to_seconds(
            self.warmup if warmup_only else self.duration
        )
        if duration_seconds is None:
            duration_seconds = 5.0 if warmup_only else 30.0

        # Per-scenario connection/stream overrides (e.g. files uses fewer to avoid OOM)
        conns = h2_scenario.connections if h2_scenario.connections is not None else self.connections
        streams = h2_scenario.streams if h2_scenario.streams is not None else self.h2_streams

        # Build h2load command
        cmd: List[str] = [
            "h2load",
            f"-c{conns}",
            f"-t{self.threads}",
            f"-m{streams}",
            f"-D{duration_seconds:.0f}",
            # Prevent indefinite hangs: kill stale connections after duration + margin
            f"-T{duration_seconds + 10:.0f}s",
        ]

        # POST body file
        if h2_scenario.body_file:
            data_path = self.script_dir / "h2_data" / h2_scenario.body_file
            if data_path.is_file():
                cmd += ["-d", str(data_path)]

        # Extra headers
        for hdr in h2_scenario.extra_headers:
            cmd += ["-H", hdr]

        # TLS: negotiate h2 via ALPN
        if use_tls:
            cmd += ["--alpn-list=h2"]

        cmd += urls

        # Process-level timeout: h2load can hang forever if all connections stall
        # during TLS handshake or if the remote server stops responding.
        self._h2load_process_timeout = duration_seconds + 60

        if warmup_only:
            print(f">>> Warm-up (h2load): {server} / {scenario_name}")
            try:
                subprocess.run(
                    cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    timeout=self._h2load_process_timeout,
                )
            except subprocess.TimeoutExpired:
                print(f"    WARNING: h2load warm-up timed out for {server} / {scenario_name}")
            return

        print(f">>> Running (h2load): {server} / {scenario_name}")
        print(f"    URL(s): {', '.join(urls)}")
        print(f"    Cmd: {' '.join(cmd)}")

        output, h2load_crashed = self._exec_h2load(cmd)
        if h2load_crashed:
            # h2load can crash (SIGABRT from libev epoll assertion) when servers
            # drop connections under heavy TLS load.  Retry with progressively
            # fewer connections so we still get usable numbers.
            for divisor in (4, 16):
                retry_conns = max(conns // divisor, 4)
                if retry_conns >= conns:
                    break
                print(f"    Retrying with -c{retry_conns} (reduced from {conns})...")
                retry_cmd = list(cmd)
                for idx, tok in enumerate(retry_cmd):
                    if tok.startswith("-c"):
                        retry_cmd[idx] = f"-c{retry_conns}"
                        break
                retry_output, retry_crashed = self._exec_h2load(retry_cmd)
                if not retry_crashed:
                    output = retry_output
                    h2load_crashed = False
                    break
                # Use whichever output has more successful requests
                retry_metrics = self._parse_h2load_output(retry_output)
                orig_metrics = self._parse_h2load_output(output)
                if int(retry_metrics.get("succeeded", 0)) > int(orig_metrics.get("succeeded", 0)):
                    output = retry_output

        metrics = self._parse_h2load_output(output)
        succeeded = int(metrics.get("succeeded", 0))

        if h2load_crashed and succeeded == 0:
            print(
                f"ERROR: h2load failed for {server} / {scenario_name}"
            )
            print(output)
            self._store_result(server, scenario_name, "-", "-", "-", latency_raw="-", timeout_errors=0)
            self._append_result_block(server, scenario_name, output, error=True)
            self._record_memory_usage(server, scenario_name)
            return
        if h2load_crashed:
            print(f"WARNING: h2load crashed but produced partial results for {server} / {scenario_name}")

        failed = int(metrics.get("failed", 0))
        errored = int(metrics.get("errored", 0))
        timeout = int(metrics.get("timeout", 0))
        non2xx = int(metrics.get("non2xx", 0))
        total_errors = failed + errored + timeout + non2xx

        if server == "aeronet" and total_errors > 0:
            self._aeronet_errors_found = True
            print(
                f"ERROR: Aeronet h2load issues (failed/errored/timeout/non2xx) = "
                f"{failed}/{errored}/{timeout}/{non2xx} for '{scenario_name}'"
            )

        succeeded = int(metrics.get("succeeded", 0))
        duration_s = metrics.get("duration_seconds")
        if duration_s and duration_s > 0 and succeeded > 0:
            success_rps = f"{succeeded / duration_s:.2f}"
        else:
            success_rps = metrics.get("rps", "-")

        self._store_result(
            server,
            scenario_name,
            success_rps,
            metrics.get("latency", "-"),
            metrics.get("transfer", "-"),
            rps_raw=metrics.get("rps", "-"),
            latency_raw=metrics.get("latency", "-"),
            timeout_errors=timeout,
        )
        print(output)
        self._append_result_block(
            server, scenario_name, output, error=(server == "aeronet" and total_errors > 0)
        )
        self._record_memory_usage(server, scenario_name)

    def _exec_h2load(self, cmd: List[str]) -> Tuple[str, bool]:
        """Run h2load and return (output, crashed).

        h2load can crash with SIGABRT (exit -6) due to a libev epoll assertion
        when servers close connections under load.  We capture whatever output
        was produced so callers can still extract partial metrics.
        A process-level timeout prevents indefinite hangs when all connections
        stall (e.g. during TLS handshake against a saturated server).
        """
        timeout = getattr(self, "_h2load_process_timeout", 120)
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, check=True, timeout=timeout)
            return result.stdout + (result.stderr or ""), False
        except subprocess.TimeoutExpired as exc:
            print(f"    WARNING: h2load process timed out after {timeout}s")
            stdout = exc.stdout or b"" if isinstance(exc.stdout, bytes) else exc.stdout or ""
            stderr = exc.stderr or b"" if isinstance(exc.stderr, bytes) else exc.stderr or ""
            if isinstance(stdout, bytes):
                stdout = stdout.decode("utf-8", errors="replace")
            if isinstance(stderr, bytes):
                stderr = stderr.decode("utf-8", errors="replace")
            return stdout + stderr, True
        except subprocess.CalledProcessError as exc:
            stdout = exc.stdout or b"" if isinstance(exc.stdout, bytes) else exc.stdout or ""
            stderr = exc.stderr or b"" if isinstance(exc.stderr, bytes) else exc.stderr or ""
            if isinstance(stdout, bytes):
                stdout = stdout.decode("utf-8", errors="replace")
            if isinstance(stderr, bytes):
                stderr = stderr.decode("utf-8", errors="replace")
            return stdout + stderr, True

    def _parse_h2load_output(self, output: str) -> Dict[str, Any]:
        """Parse h2load output into a metrics dictionary.

        h2load output format:
          finished in 10.01s, 12345.67 req/s, 56.78MB/s
          requests: 123456 total, ... succeeded, N failed, N errored, N timeout
          status codes: 123456 2xx, 0 3xx, 0 4xx, 0 5xx
          traffic: 567.89MB ...
          time for request:  123.45us  456.78us  234.56us  ...
          req/s  :  1234.56  5678.90  2345.67  ...
        """
        values: Dict[str, Any] = {
            "rps": "-",
            "latency": "-",
            "transfer": "-",
            "duration_seconds": None,
            "succeeded": 0,
            "failed": 0,
            "errored": 0,
            "timeout": 0,
            "non2xx": 0,
            "total_requests": 0,
        }

        for line in output.splitlines():
            line = line.strip()

            # "finished in 10.01s, 12345.67 req/s, 56.78MB/s"
            match = re.match(
                r"finished\s+in\s+([0-9.]+)s?,\s+([0-9.]+)\s+req/s,\s+([0-9.]+\S+)/s",
                line,
            )
            if match:
                values["duration_seconds"] = float(match.group(1))
                values["rps"] = match.group(2)
                values["transfer"] = match.group(3) + "/s"
                continue

            # "requests: 123456 total, 123456 started, 123456 done, 123400 succeeded, 56 failed, 0 errored, 0 timeout"
            if line.startswith("requests:"):
                match = re.search(r"(\d+)\s+total", line)
                if match:
                    values["total_requests"] = int(match.group(1))
                match = re.search(r"(\d+)\s+succeeded", line)
                if match:
                    values["succeeded"] = int(match.group(1))
                match = re.search(r"(\d+)\s+failed", line)
                if match:
                    values["failed"] = int(match.group(1))
                match = re.search(r"(\d+)\s+errored", line)
                if match:
                    values["errored"] = int(match.group(1))
                match = re.search(r"(\d+)\s+timeout", line)
                if match:
                    values["timeout"] = int(match.group(1))
                continue

            # "status codes: 123456 2xx, 0 3xx, 12 4xx, 0 5xx"
            if line.startswith("status codes:"):
                twox = re.search(r"(\d+)\s+2xx", line)
                threex = re.search(r"(\d+)\s+3xx", line)
                fourx = re.search(r"(\d+)\s+4xx", line)
                fivex = re.search(r"(\d+)\s+5xx", line)
                twox_count = int(twox.group(1)) if twox else 0
                non2xx = 0
                for m in (threex, fourx, fivex):
                    if m:
                        non2xx += int(m.group(1))
                values["non2xx"] = non2xx
                continue

            # "time for request:    123.45us    456.78us    234.56us ..."
            # columns: min  max  mean  sd  +/- sd
            if line.startswith("time for request:"):
                parts = line.split()
                # parts: ["time", "for", "request:", min, max, mean, sd, "+/-", "sd"]
                if len(parts) >= 6:
                    values["latency"] = parts[5]  # mean latency
                continue

        return values

    def _h2_scenario_server_args(self, server: str, scenario: str) -> List[str]:
        """Extra server args for special H2 scenarios."""
        args: List[str] = []
        h2_meta = self.H2_SCENARIOS.get(scenario)
        if not h2_meta:
            return args
        static_dir = self.script_dir / "static"
        if h2_meta.requires_static and static_dir.is_dir():
            args += ["--static", str(static_dir)]
        if scenario == "routing":
            args += ["--routes", "1000"]
        return args

    def _parse_wrk_output(self, output: str) -> Dict[str, Any]:
        values: Dict[str, Any] = {
            "rps": "-",
            "latency": "-",
            "transfer": "-",
            "non2xx": 0,
            "total_requests": 0,
            "duration_seconds": None,
        }
        non2xx = 0
        for line in output.splitlines():
            line = line.strip()
            if line.startswith("Non-2xx"):
                try:
                    non2xx = int(line.split(":", 1)[1])
                except Exception:
                    non2xx = 1
            elif "requests in" in line:
                match = re.search(r"(\d+)\s+requests\s+in\s+([0-9]*\.?[0-9]+)s", line)
                if match is not None:
                    values["total_requests"] = int(match.group(1))
                    values["duration_seconds"] = float(match.group(2))
            elif line.startswith("Requests/sec"):
                values["rps"] = line.split(":", 1)[1].strip()
            elif line.startswith("Latency") and values["latency"] == "-":
                parts = line.split()
                if len(parts) >= 2:
                    values["latency"] = parts[1]
            elif line.startswith("Transfer/sec"):
                values["transfer"] = line.split(":", 1)[1].strip()
        values["non2xx"] = non2xx
        return values

    def _extract_wrk_errors(self, output: str) -> Tuple[int, int, int, int]:
        """Parse wrk Lua script summary lines for error counts.

        Looks for lines like:
          "Errors (connect): X"
          "Errors (read): X"
          "Errors (write): X"
          "Errors (timeout): X"
        Returns a tuple: (connect, read, write, timeout). Missing lines default to 0.
        """
        e_connect = e_read = e_write = e_timeout = 0
        for line in output.splitlines():
            line = line.strip()
            if line.startswith("Errors (connect):"):
                try:
                    e_connect = int(line.split(":", 1)[1].strip())
                except Exception:
                    e_connect = e_connect or 1
            elif line.startswith("Errors (read):"):
                try:
                    e_read = int(line.split(":", 1)[1].strip())
                except Exception:
                    e_read = e_read or 1
            elif line.startswith("Errors (write):"):
                try:
                    e_write = int(line.split(":", 1)[1].strip())
                except Exception:
                    e_write = e_write or 1
            elif line.startswith("Errors (timeout):"):
                try:
                    e_timeout = int(line.split(":", 1)[1].strip())
                except Exception:
                    e_timeout = e_timeout or 1
        return e_connect, e_read, e_write, e_timeout

    def _store_result(
        self,
        server: str,
        scenario: str,
        rps: str,
        latency: str,
        transfer: str,
        *,
        rps_raw: Optional[str] = None,
        latency_raw: Optional[str] = None,
        timeout_errors: Optional[int] = None,
    ) -> None:
        key = (server, scenario)
        self.results_rps[key] = rps
        self.results_rps_raw[key] = rps if rps_raw is None else rps_raw
        self.results_latency[key] = latency
        self.results_latency_raw[key] = latency if latency_raw is None else latency_raw
        self.results_transfer[key] = transfer
        if timeout_errors is not None:
            self.results_timeouts[key] = timeout_errors

    @staticmethod
    def _duration_to_seconds(value: str) -> Optional[float]:
        text = str(value).strip()
        match = re.match(r"^([0-9]*\.?[0-9]+)\s*(us|ms|s|m|h)?$", text)
        if match is None:
            return None
        amount = float(match.group(1))
        unit = (match.group(2) or "s").lower()
        if unit == "us":
            return amount / 1_000_000.0
        if unit == "ms":
            return amount / 1000.0
        if unit == "s":
            return amount
        if unit == "m":
            return amount * 60.0
        if unit == "h":
            return amount * 3600.0
        return None

    @staticmethod
    def _latency_to_seconds(value: str) -> Optional[float]:
        text = str(value).strip().lower()
        match = re.match(r"^([0-9]*\.?[0-9]+)\s*(us|µs|μs|ms|s)$", text)
        if match is None:
            return None
        amount = float(match.group(1))
        unit = match.group(2)
        if unit in {"us", "µs", "μs"}:
            return amount / 1_000_000.0
        if unit == "ms":
            return amount / 1000.0
        return amount

    @staticmethod
    def _format_latency_seconds(seconds: float) -> str:
        if seconds >= 1.0:
            return f"{seconds:.2f}s"
        if seconds >= 0.001:
            return f"{seconds * 1000.0:.2f}ms"
        return f"{seconds * 1_000_000.0:.2f}us"

    def _compute_timeout_adjusted_latency(
        self,
        raw_latency: str,
        successful_requests: int,
        timeout_errors: int,
        timeout_seconds: float,
    ) -> str:
        if raw_latency == "-" or timeout_errors <= 0:
            return raw_latency
        raw_seconds = self._latency_to_seconds(raw_latency)
        if raw_seconds is None:
            return raw_latency
        completed = max(0, successful_requests)
        total = completed + timeout_errors
        if total <= 0:
            return raw_latency
        adjusted_seconds = (raw_seconds * completed + timeout_seconds * timeout_errors) / total
        return self._format_latency_seconds(adjusted_seconds)

    @staticmethod
    def _compute_success_rps(
        raw_rps: str,
        total_requests: int,
        total_errors: int,
        measured_duration_seconds: Optional[float],
    ) -> str:
        if measured_duration_seconds is not None and measured_duration_seconds > 0.0:
            successful = max(0, total_requests - max(0, total_errors))
            return f"{successful / measured_duration_seconds:.2f}"
        return raw_rps

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
        tool = "h2load" if self.protocol in ("h2c", "h2-tls") else "wrk"
        with self.result_file.open("w", encoding="utf-8") as fp:
            fp.write("HTTP Server Benchmark Results\n")
            fp.write("==============================\n")
            fp.write(f"Date: {time.ctime()}\n")
            fp.write(f"Protocol: {self.protocol}\n")
            fp.write(f"Tool: {tool}\n")
            fp.write(f"Threads: {self.threads}\n")
            fp.write(f"Connections: {self.connections}\n")
            fp.write(f"Duration: {self.duration}\n")
            if self.protocol in ("h2c", "h2-tls"):
                fp.write(f"H2 Streams/conn: {self.h2_streams}\n")
            fp.write(f"wrk timeout: {self.wrk_timeout}\n")
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
            "protocol": self.protocol,
            "tool": "h2load" if self.protocol in ("h2c", "h2-tls") else "wrk",
            "threads": self.threads,
            "connections": self.connections,
            "duration": self.duration,
            "warmup": self.warmup,
            "wrk_timeout": self.wrk_timeout,
            "servers": self.servers_to_test,
            "scenarios": self.scenarios_to_test,
            "results": {},
        }
        if self.protocol in ("h2c", "h2-tls"):
            summary["h2_streams"] = self.h2_streams

        for scenario in self.scenarios_to_test:
            scenario_entry = {
                "rps": {},
                "rps_raw": {},
                "latency": {},
                "latency_raw": {},
                "timeouts": {},
                "transfer": {},
                "winners": {},
            }
            best_server = self._best_server_for_scenario(scenario)
            if best_server is not None:
                scenario_entry["winners"]["rps"] = best_server
            for server in self.servers_to_test:
                key = (server, scenario)
                rps_val = self.results_rps.get(key)
                rps_raw_val = self.results_rps_raw.get(key)
                lat_val = self.results_latency.get(key)
                lat_raw_val = self.results_latency_raw.get(key)
                timeout_val = self.results_timeouts.get(key)
                xfer_val = self.results_transfer.get(key)
                if rps_val is not None:
                    scenario_entry["rps"][server] = rps_val
                if rps_raw_val is not None:
                    scenario_entry["rps_raw"][server] = rps_raw_val
                if lat_val is not None:
                    scenario_entry["latency"][server] = lat_val
                if lat_raw_val is not None:
                    scenario_entry["latency_raw"][server] = lat_raw_val
                if timeout_val is not None:
                    scenario_entry["timeouts"][server] = timeout_val
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
            ("large.bin", 25 * 1024 * 1024),
            ("medium.bin", 1 * 1024 * 1024),
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
            "(Successful responses/sec - higher is better)",
            self.rps,
            higher_is_better=True,
        )
        self._print_box(
            "LATENCY COMPARISON",
            "(Timeout-adjusted average - lower is better)",
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
    default_connections = int(os.environ.get("BENCH_CONNECTIONS", 50*default_threads))
    default_duration = os.environ.get("BENCH_DURATION", "30s")
    default_warmup = os.environ.get("BENCH_WARMUP", "5s")
    default_wrk_timeout = os.environ.get("BENCH_WRK_TIMEOUT", "10s")
    default_output = os.environ.get("BENCH_OUTPUT", "./results")
    parser = argparse.ArgumentParser(
        description="Run wrk/h2load benchmarks across multiple servers"
    )
    parser.add_argument(
        "--protocol",
        type=str,
        default="http1",
        choices=["http1", "h2c", "h2-tls"],
        help="Protocol to benchmark: http1 (wrk), h2c (h2load, cleartext), h2-tls (h2load, TLS)",
    )
    parser.add_argument(
        "--threads", type=int, default=default_threads, help="Number of wrk/h2load threads"
    )

    # Low number of connections -> Measure latency more accurately, but may not fully saturate high-performance servers
    # High number of connections -> Better for measuring max throughput, but may cause more timeouts and less accurate latency measurements
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
        "--wrk-timeout",
        type=str,
        default=default_wrk_timeout,
        help="Per-request wrk timeout used both by wrk and timeout-penalty latency adjustment (e.g. 2s)",
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
        default="all-except-python",
        help="Comma-separated list of servers (aeronet,drogon,pistache,crow,undertow,go,python,rust), or 'all' or 'all-except-python'",
    )
    parser.add_argument(
        "--scenario",
        type=str,
        default="all",
        help="Comma-separated list of scenarios (headers,body,body-codec,static,cpu,mixed,files,routing,tls)",
    )
    parser.add_argument(
        "--h2-streams",
        type=int,
        default=int(os.environ.get("BENCH_H2_STREAMS", "10")),
        help="Max concurrent HTTP/2 streams per connection (h2load -m, default: 10)",
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
