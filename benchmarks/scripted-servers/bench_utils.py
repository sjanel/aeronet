#!/usr/bin/env python3
"""Shared utilities for HTTP and WebSocket benchmark scripts."""
from __future__ import annotations

import os
import re
import resource
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
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


# --------------------------- CPU placement & saturation --------------------------- #
#
# A *server* benchmark is only meaningful when the server is the bottleneck: the load
# generator must be given enough CPU (and threads) to keep the server busy, otherwise
# the numbers measure the client and the scheduler, not the server. The helpers below
# reserve CPUs for the server and the load generator (SMT aware) and measure the CPU
# utilization of both sides, so that every result can be flagged as saturated or not.

# Minimum utilization of its reserved CPUs for a process to count as saturated.
SATURATION_THRESHOLD_PCT = 90.0


def parse_cpu_list(value: str) -> List[int]:
    """Parse a Linux CPU list ("0-3,8,10-11") into CPU IDs."""
    cpus: List[int] = []
    for part in value.strip().split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            start_text, end_text = part.split("-", 1)
            cpus.extend(range(int(start_text), int(end_text) + 1))
        else:
            cpus.append(int(part))
    return cpus


def format_cpu_list(cpus: Sequence[int]) -> str:
    """Format CPU IDs for ``taskset -c``, coalescing adjacent IDs into ranges."""
    ordered = sorted(set(cpus))
    if not ordered:
        return ""
    parts: List[str] = []
    start = previous = ordered[0]
    for cpu in ordered[1:]:
        if cpu == previous + 1:
            previous = cpu
            continue
        parts.append(str(start) if start == previous else f"{start}-{previous}")
        start = previous = cpu
    parts.append(str(start) if start == previous else f"{start}-{previous}")
    return ",".join(parts)


def available_cpus() -> List[int]:
    """CPUs this process may run on (respects Linux cpusets / affinity)."""
    if hasattr(os, "sched_getaffinity"):
        return sorted(os.sched_getaffinity(0))
    return list(range(os.cpu_count() or 1))


def linux_sibling_map(cpus: Sequence[int]) -> Dict[int, List[int]]:
    """Map each CPU to the SMT siblings (same physical core) among ``cpus``."""
    available = set(cpus)
    siblings: Dict[int, List[int]] = {}
    for cpu in cpus:
        path = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")
        try:
            group = sorted(available.intersection(parse_cpu_list(path.read_text(encoding="ascii"))))
        except (OSError, ValueError):
            group = [cpu]
        siblings[cpu] = group or [cpu]
    return siblings


@dataclass(frozen=True)
class CpuPlan:
    """CPUs reserved for the server and the load generator of a server benchmark."""

    server_cpus: List[int]
    loadgen_cpus: List[int]
    loadgen_threads: int
    # True when a load generator CPU is an SMT sibling of a server CPU (unavoidable on
    # small machines such as 4-vCPU CI runners: 2 physical cores).
    shares_server_cores: bool = False

    @property
    def pinned(self) -> bool:
        return bool(self.server_cpus) and bool(self.loadgen_cpus)

    def server_prefix(self) -> List[str]:
        return ["taskset", "-c", format_cpu_list(self.server_cpus)] if self.pinned else []

    def loadgen_prefix(self) -> List[str]:
        return ["taskset", "-c", format_cpu_list(self.loadgen_cpus)] if self.pinned else []

    def describe(self) -> str:
        if not self.pinned:
            return f"CPU pinning disabled, load generator threads: {self.loadgen_threads}"
        note = " (load generator shares SMT cores with the server)" if self.shares_server_cores else ""
        return (
            f"server CPUs {format_cpu_list(self.server_cpus)}, load generator CPUs "
            f"{format_cpu_list(self.loadgen_cpus)} ({self.loadgen_threads} threads){note}"
        )

    def to_json(self) -> Dict[str, Any]:
        return {
            "server_cpus": format_cpu_list(self.server_cpus),
            "loadgen_cpus": format_cpu_list(self.loadgen_cpus),
            "loadgen_threads": self.loadgen_threads,
            "shares_server_cores": self.shares_server_cores,
        }


# Upper bound of load generator threads per server thread when sized automatically:
# a loopback client costs about as much CPU per request as an efficient server, so
# 3x leaves headroom to saturate the server without flooding big machines.
DEFAULT_MAX_LOADGEN_RATIO = 3


def plan_server_benchmark_cpus(
    server_threads: int,
    loadgen_threads: int = 0,
    *,
    pin: bool = True,
    max_loadgen_ratio: int = DEFAULT_MAX_LOADGEN_RATIO,
    cpus: Optional[Sequence[int]] = None,
    sibling_map: Optional[Dict[int, List[int]]] = None,
) -> CpuPlan:
    """Reserve CPUs so that the load generator can always saturate the server.

    The server gets ``server_threads`` CPUs, one per physical core when possible. The
    load generator gets every remaining CPU it needs, CPUs of other physical cores
    first and SMT siblings of the server last. ``loadgen_threads`` <= 0 sizes it
    automatically: all remaining CPUs, up to ``max_loadgen_ratio`` per server thread.
    """
    server_threads = max(1, server_threads)
    cpus = list(cpus) if cpus is not None else available_cpus()
    auto_threads = max(1, min(len(cpus) - server_threads, max_loadgen_ratio * server_threads))
    wanted_threads = loadgen_threads if loadgen_threads > 0 else auto_threads
    if not pin or len(cpus) < server_threads + 1:
        return CpuPlan([], [], max(1, wanted_threads))
    sibling_map = sibling_map if sibling_map is not None else linux_sibling_map(cpus)

    # Physical cores, as groups of SMT siblings ordered by their first CPU.
    groups: List[List[int]] = []
    seen: set = set()
    for cpu in cpus:
        if cpu in seen:
            continue
        group = sorted(set(sibling_map.get(cpu, [cpu])).intersection(cpus)) or [cpu]
        seen.update(group)
        groups.append(group)

    server_cpus = [group[0] for group in groups[:server_threads]]
    if len(server_cpus) < server_threads:
        extra = [cpu for group in groups for cpu in group[1:]]
        server_cpus += extra[: server_threads - len(server_cpus)]
    server_set = set(server_cpus)
    server_siblings = {cpu for group in groups if server_set.intersection(group) for cpu in group} - server_set
    # One CPU per free physical core first, then their SMT siblings, then the server cores' siblings.
    free_groups = [group for group in groups if not server_set.intersection(group)]
    depth = max((len(group) for group in free_groups), default=0)
    candidates = [group[level] for level in range(depth) for group in free_groups if level < len(group)]
    candidates += sorted(server_siblings)
    if not candidates:
        return CpuPlan([], [], max(1, wanted_threads))
    threads = max(1, min(wanted_threads, len(candidates)))
    loadgen_cpus = candidates[:threads]
    return CpuPlan(
        server_cpus=server_cpus,
        loadgen_cpus=loadgen_cpus,
        loadgen_threads=threads,
        shares_server_cores=bool(server_siblings.intersection(loadgen_cpus)),
    )


_CLOCK_TICKS = os.sysconf("SC_CLK_TCK") if hasattr(os, "sysconf") else 100


def process_tree_pids(pid: int) -> List[int]:
    """``pid`` and its descendants (multi-process servers), from /proc."""
    children: Dict[int, List[int]] = {}
    try:
        entries = list(Path("/proc").iterdir())
    except OSError:
        return [pid]
    for entry in entries:
        if not entry.name.isdigit():
            continue
        try:
            stat = entry.joinpath("stat").read_text()
        except OSError:
            continue
        fields = stat[stat.rfind(")") + 2:].split()
        if len(fields) > 1:
            children.setdefault(int(fields[1]), []).append(int(entry.name))
    tree: List[int] = []
    to_visit = [pid]
    while to_visit:
        current = to_visit.pop()
        if current in tree:
            continue
        tree.append(current)
        to_visit.extend(children.get(current, []))
    return tree


def process_tree_cpu_seconds(pid: int) -> Optional[float]:
    """User + system CPU time consumed so far by ``pid`` and its descendants."""
    total = 0.0
    found = False
    for proc_pid in process_tree_pids(pid):
        try:
            stat = Path(f"/proc/{proc_pid}/stat").read_text()
        except OSError:
            continue
        fields = stat[stat.rfind(")") + 2:].split()
        # fields[0] is the state (3rd field of /proc/<pid>/stat): utime / stime are the 14th / 15th.
        if len(fields) > 12:
            total += (int(fields[11]) + int(fields[12])) / _CLOCK_TICKS
            found = True
    return total if found else None


def children_cpu_seconds() -> float:
    """User + system CPU time of the terminated and waited-for children of this process."""
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return usage.ru_utime + usage.ru_stime


@dataclass
class CpuUsage:
    """CPU utilization over a measurement window, in % of the CPUs reserved for each side."""

    server_pct: Optional[float] = None
    loadgen_pct: Optional[float] = None

    def server_saturated(self) -> Optional[bool]:
        return None if self.server_pct is None else self.server_pct >= SATURATION_THRESHOLD_PCT


@dataclass
class CpuMeter:
    """Measure the CPU utilization of a server process tree and of the load generator.

    The load generator must be a child process run (and waited for) between ``start``
    and ``stop``: its usage is taken from ``getrusage(RUSAGE_CHILDREN)``.
    """

    server_pid: Optional[int]
    server_cpus: int
    loadgen_cpus: int
    _start_wall: float = field(default=0.0, init=False)
    _start_server: Optional[float] = field(default=None, init=False)
    _start_children: float = field(default=0.0, init=False)

    def start(self) -> None:
        self._start_server = process_tree_cpu_seconds(self.server_pid) if self.server_pid else None
        self._start_children = children_cpu_seconds()
        self._start_wall = time.monotonic()

    def stop(self) -> CpuUsage:
        wall = time.monotonic() - self._start_wall
        usage = CpuUsage()
        if wall <= 0:
            return usage
        if self.server_pid and self._start_server is not None:
            end_server = process_tree_cpu_seconds(self.server_pid)
            if end_server is not None:
                usage.server_pct = 100.0 * (end_server - self._start_server) / (wall * max(1, self.server_cpus))
        usage.loadgen_pct = (
            100.0 * (children_cpu_seconds() - self._start_children) / (wall * max(1, self.loadgen_cpus))
        )
        return usage


def duration_to_seconds(value: str) -> Optional[float]:
    """Parse a load generator duration ("500ms", "30s", "2m", bare seconds) into seconds."""
    match = re.match(r"^([0-9]*\.?[0-9]+)\s*(us|ms|s|m|h)?$", str(value).strip())
    if match is None:
        return None
    factors = {"us": 1e-6, "ms": 1e-3, "s": 1.0, "m": 60.0, "h": 3600.0}
    return float(match.group(1)) * factors[(match.group(2) or "s").lower()]


def format_pct(value: Optional[float]) -> str:
    return "-" if value is None else f"{value:.0f}%"
