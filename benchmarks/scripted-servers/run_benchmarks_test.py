#!/usr/bin/env python3
"""Focused tests for the scripted-server benchmark orchestrator."""
from __future__ import annotations

import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("run_benchmarks.py")
SPEC = importlib.util.spec_from_file_location("run_benchmarks", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)

import bench_utils  # noqa: E402  (same directory as run_benchmarks.py)

RENDER_SPEC = importlib.util.spec_from_file_location(
    "render_benchmarks_html", Path(__file__).with_name("render_benchmarks_html.py")
)
assert RENDER_SPEC is not None and RENDER_SPEC.loader is not None
renderer = importlib.util.module_from_spec(RENDER_SPEC)
sys.modules[RENDER_SPEC.name] = renderer
RENDER_SPEC.loader.exec_module(renderer)

WS_SPEC = importlib.util.spec_from_file_location("run_ws_benchmarks", Path(__file__).with_name("run_ws_benchmarks.py"))
assert WS_SPEC is not None and WS_SPEC.loader is not None
ws_runner = importlib.util.module_from_spec(WS_SPEC)
sys.modules[WS_SPEC.name] = ws_runner
WS_SPEC.loader.exec_module(ws_runner)

# 2 physical cores x 2 SMT threads, as on 4-vCPU CI runners.
SMT_4_CPUS = {0: [0, 1], 1: [0, 1], 2: [2, 3], 3: [2, 3]}


class WrkExecutionTest(unittest.TestCase):
    def test_returns_stdout_for_clean_run(self) -> None:
        completed = subprocess.CompletedProcess(
            args=["wrk"], returncode=0, stdout="Requests/sec: 123\n", stderr=""
        )
        with mock.patch.object(runner.subprocess, "run", return_value=completed) as run:
            output = runner._run_wrk(["wrk", "http://127.0.0.1:8080/"])

        self.assertEqual(output, completed.stdout)
        run.assert_called_once_with(
            ["wrk", "http://127.0.0.1:8080/"], capture_output=True, text=True
        )

    def test_rejects_stderr_when_wrk_exits_successfully(self) -> None:
        completed = subprocess.CompletedProcess(
            args=["wrk"],
            returncode=0,
            stdout="Requests/sec: 123\n",
            stderr="mixed_workload.lua: syntax error near '{'\n",
        )
        with mock.patch.object(runner.subprocess, "run", return_value=completed):
            with self.assertRaisesRegex(runner.BenchmarkError, "syntax error"):
                runner._run_wrk(["wrk", "-s", "mixed_workload.lua"])

    def test_preserves_nonzero_exit_as_process_failure(self) -> None:
        completed = subprocess.CompletedProcess(
            args=["wrk"], returncode=2, stdout="", stderr="unable to connect\n"
        )
        with mock.patch.object(runner.subprocess, "run", return_value=completed):
            with self.assertRaises(subprocess.CalledProcessError) as raised:
                runner._run_wrk(["wrk"])

        self.assertEqual(raised.exception.returncode, 2)



class CpuListTest(unittest.TestCase):
    def test_round_trip(self) -> None:
        self.assertEqual(bench_utils.parse_cpu_list("0-2,5, 7-8"), [0, 1, 2, 5, 7, 8])
        self.assertEqual(bench_utils.format_cpu_list([8, 0, 1, 2, 5, 7]), "0-2,5,7-8")
        self.assertEqual(bench_utils.format_cpu_list([]), "")


class CpuPlanTest(unittest.TestCase):
    def test_load_generator_gets_free_cores_first_then_server_siblings(self) -> None:
        plan = bench_utils.plan_server_benchmark_cpus(1, cpus=[0, 1, 2, 3], sibling_map=SMT_4_CPUS)
        self.assertEqual(plan.server_cpus, [0])
        self.assertEqual(plan.loadgen_cpus, [2, 3, 1])
        self.assertEqual(plan.loadgen_threads, 3)
        self.assertTrue(plan.shares_server_cores)
        self.assertEqual(plan.server_prefix(), ["taskset", "-c", "0"])
        self.assertEqual(plan.loadgen_prefix(), ["taskset", "-c", "1-3"])

    def test_one_cpu_per_physical_core_before_siblings(self) -> None:
        siblings = {cpu: [cpu % 4, cpu % 4 + 4] for cpu in range(8)}
        plan = bench_utils.plan_server_benchmark_cpus(1, cpus=list(range(8)), sibling_map=siblings)
        self.assertEqual(plan.server_cpus, [0])
        self.assertEqual(plan.loadgen_cpus, [1, 2, 3])
        self.assertFalse(plan.shares_server_cores)

    def test_automatic_sizing_is_capped_relative_to_server_threads(self) -> None:
        plan = bench_utils.plan_server_benchmark_cpus(2, cpus=list(range(24)), sibling_map={c: [c] for c in range(24)})
        self.assertEqual(plan.server_cpus, [0, 1])
        self.assertEqual(plan.loadgen_threads, 2 * bench_utils.DEFAULT_MAX_LOADGEN_RATIO)

    def test_explicit_load_generator_threads(self) -> None:
        plan = bench_utils.plan_server_benchmark_cpus(
            1, 8, cpus=list(range(16)), sibling_map={c: [c] for c in range(16)}
        )
        self.assertEqual(plan.loadgen_threads, 8)
        self.assertEqual(plan.loadgen_cpus, list(range(1, 9)))

    def test_more_server_threads_than_physical_cores(self) -> None:
        plan = bench_utils.plan_server_benchmark_cpus(3, cpus=[0, 1, 2, 3], sibling_map=SMT_4_CPUS)
        self.assertEqual(plan.server_cpus, [0, 2, 1])
        self.assertEqual(plan.loadgen_cpus, [3])

    def test_no_pinning_without_spare_cpu_or_when_disabled(self) -> None:
        plan = bench_utils.plan_server_benchmark_cpus(1, cpus=[0], sibling_map={0: [0]})
        self.assertFalse(plan.pinned)
        self.assertEqual(plan.server_prefix(), [])
        self.assertEqual(plan.loadgen_prefix(), [])
        self.assertEqual(plan.loadgen_threads, 1)
        disabled = bench_utils.plan_server_benchmark_cpus(1, 4, pin=False, cpus=[0, 1, 2, 3])
        self.assertFalse(disabled.pinned)
        self.assertEqual(disabled.loadgen_threads, 4)
        self.assertIn("disabled", disabled.describe())


class DurationTest(unittest.TestCase):
    def test_parses_load_generator_durations(self) -> None:
        self.assertEqual(bench_utils.duration_to_seconds("30s"), 30.0)
        self.assertEqual(bench_utils.duration_to_seconds("500ms"), 0.5)
        self.assertEqual(bench_utils.duration_to_seconds("2m"), 120.0)
        self.assertEqual(bench_utils.duration_to_seconds("1h"), 3600.0)
        self.assertEqual(bench_utils.duration_to_seconds("250us"), 0.00025)
        self.assertEqual(bench_utils.duration_to_seconds("1.5"), 1.5)
        self.assertIsNone(bench_utils.duration_to_seconds("soon"))


class CpuMeterTest(unittest.TestCase):
    def test_measures_server_process_and_waited_children(self) -> None:
        meter = bench_utils.CpuMeter(server_pid=os.getpid(), server_cpus=1, loadgen_cpus=1)
        meter.start()
        subprocess.run(
            [sys.executable, "-c", "import time\nend = time.process_time() + 0.3\nwhile time.process_time() < end: pass"],
            check=True,
        )
        usage = meter.stop()
        self.assertIsNotNone(usage.loadgen_pct)
        self.assertGreater(usage.loadgen_pct, 30.0)
        self.assertIsNotNone(usage.server_pct)
        self.assertIsNotNone(usage.server_saturated())

    def test_unknown_server_process(self) -> None:
        self.assertIsNone(bench_utils.process_tree_cpu_seconds(2**30))
        usage = bench_utils.CpuUsage()
        self.assertIsNone(usage.server_saturated())
        self.assertEqual(bench_utils.format_pct(None), "-")
        self.assertEqual(bench_utils.format_pct(97.4), "97%")


def _summary(server_cpu: dict | None) -> dict:
    results = {
        "static": {"rps": {"aeronet": "100", "drogon": "90"}, "latency": {}, "transfer": {}},
        "files": {"rps": {"aeronet": "10", "drogon": "9"}, "latency": {}, "transfer": {}},
    }
    if server_cpu is not None:
        for scenario, values in server_cpu.items():
            results[scenario]["server_cpu"] = values
    return {
        "protocol": "http1",
        "tool": "wrk",
        "threads": 1,
        "loadgen_threads": 3,
        "connections": 100,
        "servers": ["aeronet", "drogon"],
        "scenarios": ["static", "files"],
        "results": results,
    }


class RenderServerCpuTest(unittest.TestCase):
    def test_flags_unsaturated_measurements(self) -> None:
        summary = _summary({"static": {"aeronet": 99.0, "drogon": 98.0}, "files": {"aeronet": 64.0}})
        renderer._validate_summary_schema(summary, "test")
        self.assertIn("server_cpu", [metric[0] for metric in renderer._get_metrics(summary)])
        self.assertEqual(renderer._unsaturated_measurements(summary), ["aeronet/files (64%)"])
        html = renderer.render_html([summary])
        self.assertIn("Not saturated measurements", html)
        self.assertIn("aeronet/files (64%)", html)
        self.assertIn("unsaturated-cell", html)
        self.assertIn("Load generator threads", html)

    def test_saturated_run_has_no_warning(self) -> None:
        summary = _summary({"static": {"aeronet": 99.0, "drogon": 98.0}, "files": {"aeronet": 95.0, "drogon": 91.0}})
        html = renderer.render_html([summary])
        self.assertNotIn("Not saturated measurements", html)

    def test_summaries_without_cpu_data_keep_rendering(self) -> None:
        summary = _summary(None)
        del summary["loadgen_threads"]
        renderer._validate_summary_schema(summary, "test")
        self.assertNotIn("server_cpu", [metric[0] for metric in renderer._get_metrics(summary)])
        html = renderer.render_html([summary])
        self.assertNotIn("Server CPU", html)
        self.assertNotIn("Load generator threads", html)


    def test_client_side_summary_checks_client_saturation(self) -> None:
        summary = _summary(None)
        summary["measured_side"] = "client"
        summary["results"]["static"]["client_cpu"] = {"aeronet": 100.0, "drogon": 99.0}
        summary["results"]["files"]["client_cpu"] = {"aeronet": 57.0}
        summary["results"]["files"]["server_cpu"] = {"aeronet": 20.0}  # informative only on the client side
        renderer._validate_summary_schema(summary, "test")
        metric_keys = [metric[0] for metric in renderer._get_metrics(summary)]
        self.assertIn("client_cpu", metric_keys)
        self.assertNotIn("server_cpu", metric_keys)
        self.assertEqual(renderer._unsaturated_measurements(summary), ["aeronet/files (57%)"])
        html = renderer.render_html([summary])
        self.assertIn("Client CPU", html)
        self.assertIn("the client used less than 90%", html)


def _ws_runner(tmp_dir: Path, **attrs) -> "ws_runner.WsBenchmarkRunner":
    """A WsBenchmarkRunner with only the attributes the tested methods use (no server is started)."""
    instance = object.__new__(ws_runner.WsBenchmarkRunner)
    instance.build_dir = tmp_dir
    instance.vus = 50
    instance.pipeline_depth = 1
    instance.duration = "5s"
    instance.warmup = "2s"
    instance.server_threads = 1
    instance.cpu_plan = bench_utils.CpuPlan([4], [5, 16, 17], 3, shares_server_cores=True)
    instance.loadgen_threads = 3
    for name, value in attrs.items():
        setattr(instance, name, value)
    return instance


class WsLoadgenTest(unittest.TestCase):
    def test_every_scenario_has_a_loadgen_mapping(self) -> None:
        self.assertEqual(set(ws_runner.LOADGEN_SCENARIOS), set(ws_runner.K6_SCENARIOS))

    def test_command_pins_and_maps_the_scenario(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            runner_ = _ws_runner(Path(tmp), pipeline_depth=4)
            command = runner_._loadgen_command("aeronet", "compression")
            self.assertEqual(command[:3], ["taskset", "-c", "5,16-17"])
            self.assertEqual(command[3], str(Path(tmp) / "ws-loadgen"))
            self.assertEqual(command[command.index("--path") + 1], "/ws-compressed")
            self.assertEqual(command[command.index("--port") + 1], "8080")
            self.assertEqual(command[command.index("--threads") + 1], "3")
            self.assertEqual(command[command.index("--pipeline") + 1], "4")
            self.assertEqual(command[-4:], ["--mode", "echo", "--json-payload", "--compress"])

            large = runner_._loadgen_command("uwebsockets", "echo-large")
            self.assertEqual(large[large.index("--pipeline") + 1], "1")  # one 64 KB message in flight
            self.assertEqual(large[large.index("--path") + 1], "/ws-uncompressed")

            few = _ws_runner(Path(tmp), vus=2)._loadgen_command("aeronet", "churn")
            self.assertEqual(few[few.index("--threads") + 1], "2")  # at least one connection per thread

    def test_metrics_use_the_report_metric_names(self) -> None:
        data = {
            "messages": 1000, "sessions": 40, "errors": 2, "rate": 200.0, "sessions_rate": 8.0,
            "compression_refused": 1, "latency_us": {"avg": 150.0, "p50": 128, "p90": 192, "p95": 256, "p99": 512,
                                                     "max": 2048},
        }
        echo = ws_runner.WsBenchmarkRunner._loadgen_metrics("echo-small", data)
        self.assertEqual(echo["ws_messages_sent"], {"count": 1000.0, "rate": 200.0})
        self.assertEqual(echo["ws_echo_rtt_ms"]["p95"], 0.256)
        self.assertEqual(echo["checks"]["fails"], 3.0)  # errors + refused compression offers
        self.assertEqual(ws_runner.WsBenchmarkRunner._primary_throughput_rate(echo), 200.0)

        ping = ws_runner.WsBenchmarkRunner._loadgen_metrics("ping-pong", data)
        self.assertIn("ws_pings_sent", ping)
        self.assertIn("ws_ping_rtt_ms", ping)

        churn = ws_runner.WsBenchmarkRunner._loadgen_metrics("churn", data)
        self.assertEqual(churn["ws_sessions"], {"count": 40.0, "rate": 8.0})
        self.assertEqual(ws_runner.WsBenchmarkRunner._primary_latency(churn)["med"], 0.128)

        empty = ws_runner.WsBenchmarkRunner._loadgen_metrics("mix", {})
        self.assertEqual(empty["checks"]["value"], 0.0)

    def test_parses_the_last_json_line(self) -> None:
        parse = ws_runner.WsBenchmarkRunner._parse_loadgen_output
        self.assertEqual(parse('warning\n{"rate": 1.0}\n'), {"rate": 1.0})
        self.assertIsNone(parse("no result\n"))
        self.assertIsNone(parse("{not json\n"))
        self.assertIsNone(parse("[1, 2]\n"))

    def test_tool_resolution(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            runner_ = _ws_runner(Path(tmp))
            self.assertEqual(runner_._resolve_tool("k6"), "k6")
            with self.assertRaises(ws_runner.WsBenchmarkError):
                runner_._resolve_tool("ws-loadgen")
            with self.assertRaises(ws_runner.WsBenchmarkError):
                runner_._resolve_tool("wrk")
            with redirect_stdout(io.StringIO()):
                self.assertEqual(runner_._resolve_tool("auto"), "k6")  # falls back when not built
            (Path(tmp) / "ws-loadgen").touch()
            self.assertEqual(runner_._resolve_tool("auto"), "ws-loadgen")
            self.assertEqual(runner_._resolve_tool("ws-loadgen"), "ws-loadgen")


if __name__ == "__main__":
    unittest.main()
