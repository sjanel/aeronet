#!/usr/bin/env python3
"""Shared utilities for HTTP and WebSocket benchmark scripts."""
from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple


@dataclass
class PerfRecording:
    process: subprocess.Popen
    output_dir: Path
    data_path: Path
    log_fp: Any


class PerfProfiler:
    """Record and post-process benchmark processes through profile_benchmark.sh."""

    def __init__(
        self,
        output_dir: Path,
        *,
        frequency: int,
        call_graph: str,
        install_flamegraph: bool,
        open_hotspot: bool,
    ) -> None:
        if frequency <= 0:
            raise RuntimeError("perf sampling frequency must be a positive integer")
        if call_graph not in {"dwarf", "fp", "lbr"}:
            raise RuntimeError(f"unsupported perf call graph mode: {call_graph}")
        self.output_dir = output_dir
        self.frequency = frequency
        self.call_graph = call_graph
        self.install_flamegraph = install_flamegraph
        self.open_hotspot = open_hotspot
        self.helper = Path(__file__).resolve().parents[2] / "scripts/profile_benchmark.sh"
        if not self.helper.is_file() or not os.access(self.helper, os.X_OK):
            raise RuntimeError(f"Profiling helper is missing or not executable: {self.helper}")

    def check_permissions(self) -> None:
        perf = shutil.which("perf")
        if perf is None:
            raise RuntimeError("perf is not installed or is not available in PATH")
        check = subprocess.run(
            [perf, "stat", "--event", "cycles", "--", "true"],
            capture_output=True,
            text=True,
        )
        if check.returncode == 0:
            return
        paranoid = "unknown"
        paranoid_path = Path("/proc/sys/kernel/perf_event_paranoid")
        if paranoid_path.is_file():
            paranoid = paranoid_path.read_text(encoding="ascii").strip()
        detail = [line.strip() for line in check.stderr.splitlines() if line.strip()]
        reason = next(
            (
                line
                for line in detail
                if "permission" in line.lower() or "access to performance" in line.lower()
            ),
            detail[0] if detail else "perf stat failed",
        )
        raise RuntimeError(
            f"perf cannot access CPU events (kernel.perf_event_paranoid={paranoid}): {reason}\n"
            "Enable scripted profiling before the run, for example:\n"
            "  sudo sysctl kernel.perf_event_paranoid=1"
        )

    def wrap_command(
        self, command: Sequence[str], artifact_parts: Sequence[str]
    ) -> Tuple[List[str], Path]:
        artifact_dir = self._artifact_dir(artifact_parts)
        data_path = artifact_dir / "perf.data"
        return [
            str(self.helper),
            "--record-only",
            "--freq", str(self.frequency),
            "--call-graph", self.call_graph,
            "--data", str(data_path),
            "--",
            *command,
        ], artifact_dir

    def start(self, pid: int, artifact_parts: Sequence[str]) -> PerfRecording:
        artifact_dir = self._artifact_dir(artifact_parts)
        data_path = artifact_dir / "perf.data"
        log_path = artifact_dir / "perf-record.log"
        log_fp = log_path.open("w", encoding="utf-8", errors="replace")
        process = subprocess.Popen(
            [
                str(self.helper),
                "--record-only",
                "--freq", str(self.frequency),
                "--call-graph", self.call_graph,
                "--data", str(data_path),
                "--pid", str(pid),
            ],
            stdout=log_fp,
            stderr=subprocess.STDOUT,
        )
        time.sleep(0.15)
        if process.poll() is not None:
            log_fp.close()
            detail = log_path.read_text(encoding="utf-8", errors="replace").strip()
            raise RuntimeError(f"perf failed to attach to PID {pid}:\n{detail}")
        return PerfRecording(process, artifact_dir, data_path, log_fp)

    def stop(self, recording: PerfRecording) -> Path:
        process = recording.process
        if process.poll() is None:
            process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        recording.log_fp.close()
        return self.process(recording.output_dir)

    def process(self, artifact_dir: Path) -> Path:
        data_path = artifact_dir / "perf.data"
        if not data_path.is_file() or data_path.stat().st_size == 0:
            log_path = artifact_dir / "perf-record.log"
            detail = ""
            if log_path.is_file():
                detail = log_path.read_text(encoding="utf-8", errors="replace").strip()
            suffix = f":\n{detail}" if detail else ""
            raise RuntimeError(f"perf produced no data at {data_path}{suffix}")
        command = [
            str(self.helper),
            "--input", str(data_path),
            "--output-dir", str(artifact_dir),
        ]
        if self.install_flamegraph:
            command.append("--install-flamegraph")
        if self.open_hotspot:
            command.append("--hotspot")
        completed = subprocess.run(command)
        if completed.returncode != 0:
            raise RuntimeError(
                f"profile post-processing failed for {data_path} (exit {completed.returncode})"
            )
        print(f"    Profile: {artifact_dir}", file=sys.stderr)
        return artifact_dir

    def _artifact_dir(self, artifact_parts: Sequence[str]) -> Path:
        artifact_dir = self.output_dir.joinpath(*artifact_parts)
        artifact_dir.mkdir(parents=True, exist_ok=True)
        return artifact_dir


def format_rps(value: Any) -> str:
    """Format an RPS / rate value for display (e.g. 12345 → '12,345')."""
    if value is None or value == "-" or value == "":
        return "-"
    try:
        return f"{int(float(value)):,}"
    except (ValueError, TypeError):
        return str(value)


class TablePrinter:
    """Pretty-print benchmark results in a boxed table."""

    def __init__(
        self,
        servers: List[str],
        scenarios: List[str],
        metrics: List[Tuple[str, str, Dict[Tuple[str, str], str], bool]],
    ) -> None:
        """
        Args:
            servers: List of server names.
            scenarios: List of scenario names.
            metrics: List of (title, subtitle, data_dict, higher_is_better) tuples.
                     data_dict is keyed by (server, scenario) → display string.
        """
        self.servers = servers
        self.scenarios = scenarios
        self.metrics = metrics

    def print_all(self) -> None:
        for title, subtitle, data, higher_is_better in self.metrics:
            self._print_box(title, subtitle, data, higher_is_better=higher_is_better)

    def _print_box(
        self,
        title: str,
        subtitle: str,
        data: Dict[Tuple[str, str], str],
        higher_is_better: bool,
    ) -> None:
        scenario_width = max(12, max((len(s) for s in self.scenarios), default=12) + 1)
        cell_width = max(14, max((len(s) for s in self.servers), default=14) + 1)
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
    def _to_numeric(value: str, _higher_is_better: bool) -> Optional[float]:
        try:
            cleaned = value.replace(",", "")
            # Data-rate values (e.g. h2load's "843.95KB/s") end in "/s", which also ends in the
            # bare letter "s" — strip the rate suffix *before* the latency check below, otherwise
            # "KB/s" / "MB/s" / "GB/s" get treated as an unrecognized time unit (scale 1) and
            # byte-size values of different magnitudes get compared as if they were the same unit.
            is_rate = cleaned.endswith("/s")
            if is_rate:
                cleaned = cleaned[: -len("/s")]
            if is_rate or any(cleaned.endswith(unit) for unit in ("KB", "MB", "GB", "B")):
                suffix = "".join(ch for ch in cleaned if not ch.isdigit() and ch != ".")
                number = float("".join(ch for ch in cleaned if ch.isdigit() or ch == "."))
                scale = {"B": 1, "KB": 1024, "MB": 1_048_576, "GB": 1_073_741_824}.get(
                    suffix, 1
                )
                return number * scale
            if any(cleaned.endswith(unit) for unit in ("us", "ms", "s")):
                suffix = "".join(ch for ch in cleaned if not ch.isdigit() and ch != ".")
                number = float("".join(ch for ch in cleaned if ch.isdigit() or ch == "."))
                scale = {"us": 1, "ms": 1000, "s": 1_000_000}.get(suffix, 1)
                return number * scale
            return float(cleaned)
        except ValueError:
            return None
