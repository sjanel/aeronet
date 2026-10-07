#!/usr/bin/env python3
"""Focused tests for the scripted-client benchmark orchestrator."""
from __future__ import annotations

import importlib.util
import io
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("run_client_benchmarks.py")
SPEC = importlib.util.spec_from_file_location("run_client_benchmarks", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class CpuPartitionTest(unittest.TestCase):
    def test_cpu_list_round_trip(self) -> None:
        self.assertEqual(runner._parse_cpu_list("0-3,8,10-11"), [0, 1, 2, 3, 8, 10, 11])
        self.assertEqual(runner._format_cpu_list([11, 2, 1, 0, 10, 8]), "0-2,8,10-11")

    def test_client_uses_separate_physical_cores(self) -> None:
        cpus = list(range(8))
        sibling_map = {
            0: [0, 4], 1: [1, 5], 2: [2, 6], 3: [3, 7],
            4: [0, 4], 5: [1, 5], 6: [2, 6], 7: [3, 7],
        }
        client, server = runner._partition_cpu_sets(cpus, 2, sibling_map)
        self.assertEqual(client, [0, 1])
        self.assertEqual(server, [2, 3, 6, 7])

    def test_pinning_is_disabled_without_a_server_core(self) -> None:
        cpus = [0, 1, 2, 3]
        sibling_map = {cpu: [cpu] for cpu in cpus}
        self.assertEqual(runner._partition_cpu_sets(cpus, 4, sibling_map), ([], []))


class RepeatSelectionTest(unittest.TestCase):
    def test_selects_whole_lower_median_sample(self) -> None:
        samples = [
            {"rps": 400.0, "p99_us": 4.0},
            {"rps": 100.0, "p99_us": 1.0},
            {"rps": 300.0, "p99_us": 3.0},
            {"rps": 200.0, "p99_us": 2.0},
        ]
        with redirect_stdout(io.StringIO()):
            selected = runner._select_median_result(samples)
        self.assertIs(selected, samples[3])
        self.assertEqual(selected["p99_us"], 2.0)

    def test_rejects_zero_successful_samples(self) -> None:
        with self.assertRaises(runner.BenchError):
            runner._select_median_result([])

    def test_driver_uses_connection_count_and_sample_profile_path(self) -> None:
        class Profiler:
            def __init__(self) -> None:
                self.parts = []

            def wrap_command(self, command, parts):
                self.parts = parts
                return command, Path("/tmp/profile")

            def process(self, _profile_dir) -> None:
                pass

        profiler = Profiler()
        process = mock.Mock(returncode=0)
        process.communicate.return_value = ('{"rps":123.0}\n', "")
        with mock.patch.object(runner.subprocess, "Popen", return_value=process) as popen:
            result = runner.run_driver(
                Path("/tmp/curl-bench-client"), "http://127.0.0.1:8090", "small-get",
                8, "1s", "100ms", "http1", profiler,
                command_prefix=("taskset", "-c", "2-3"), sample=1, repeat=3,
            )

        self.assertEqual(result, {"rps": 123.0})
        self.assertEqual(profiler.parts, ["http1", "curl", "small-get", "sample-2"])
        command = popen.call_args.args[0]
        self.assertEqual(command[:4], ["taskset", "-c", "2-3", "/tmp/curl-bench-client"])
        threads_index = command.index("--threads")
        self.assertEqual(command[threads_index + 1], "8")


    def test_driver_reports_client_and_server_cpu_usage(self) -> None:
        process = mock.Mock(returncode=0)
        process.communicate.return_value = ('{"rps":1.0,"cpu_s":1.8,"duration_s":2.0}\n', "")
        utils = runner._bench_utils()
        with (
            mock.patch.object(runner.subprocess, "Popen", return_value=process),
            mock.patch.object(utils, "process_tree_cpu_seconds", side_effect=[10.0, 11.0]),
            mock.patch.object(runner.time, "sleep") as sleep,
            mock.patch.object(runner.time, "monotonic", side_effect=[100.0, 102.0]),
        ):
            result = runner.run_driver(
                Path("/tmp/aeronet-bench-client"), "http://127.0.0.1:8090", "small-get",
                3, "2s", "500ms", "http1", server_pid=1234, client_cpus=1, server_cpus=2,
            )

        sleep.assert_called_once_with(0.5)  # the server is sampled after the driver warmup
        self.assertAlmostEqual(result["client_cpu_pct"], 90.0)
        self.assertAlmostEqual(result["server_cpu_pct"], 25.0)  # 1 CPU second over 2 s on 2 CPUs
        self.assertTrue(runner._client_saturated(result))
        self.assertIn("client 90% of 1, server 25% of 2", runner._format_cpu_usage(result, 1, 2))

    def test_driver_timeout_kills_the_driver(self) -> None:
        process = mock.Mock(returncode=None)
        process.communicate.side_effect = [runner.subprocess.TimeoutExpired("driver", 600), ("", "")]
        with mock.patch.object(runner.subprocess, "Popen", return_value=process), redirect_stdout(io.StringIO()):
            result = runner.run_driver(
                Path("/tmp/aeronet-bench-client"), "http://127.0.0.1:8090", "small-get", 3, "1s", "0s", "http1",
            )
        self.assertIsNone(result)
        process.kill.assert_called_once()


class SaturationTest(unittest.TestCase):
    def test_unsaturated_client_is_flagged(self) -> None:
        result = {"cpu_s": 0.6, "duration_s": 1.0}
        runner._add_cpu_usage(result, None, 0.0, 1, 2)
        self.assertAlmostEqual(result["client_cpu_pct"], 60.0)
        self.assertNotIn("server_cpu_pct", result)
        self.assertFalse(runner._client_saturated(result))
        self.assertIsNone(runner._client_saturated({}))

    def test_pages_summary_records_cpu_usage_for_the_client_side(self) -> None:
        results = [{
            "client": "aeronet", "scenario": "small-get", "rps": 10.0, "avg_us": 5.0, "bytes": 1024,
            "rss_kb": 2048, "client_cpu_pct": 99.04, "server_cpu_pct": 40.0,
        }]
        meta = {
            "protocol": "http1", "date": "now", "repeat": 1, "threads": 1, "connections": 3, "server_threads": 2,
            "duration": "1s", "warmup": "0s",
        }
        summary = runner.to_pages_summary(results, meta, ["small-get"], ["aeronet"])
        self.assertEqual(summary["measured_side"], "client")
        self.assertEqual(summary["results"]["small-get"]["client_cpu"], {"aeronet": 99.0})
        self.assertEqual(summary["results"]["small-get"]["server_cpu"], {"aeronet": 40.0})
        self.assertEqual(summary["saturation_threshold_pct"], runner._bench_utils().SATURATION_THRESHOLD_PCT)


class ConcurrencyTest(unittest.TestCase):
    def test_default_connections_keep_client_cpus_busy(self) -> None:
        self.assertEqual(runner._default_connections(1), 3)
        self.assertEqual(runner._default_connections(4), 12)
        self.assertEqual(runner._default_connections(0), 3)

    def test_server_starts_in_prebuilt_client_benchmark_mode(self) -> None:
        process = mock.Mock()
        with (
            mock.patch.object(runner.subprocess, "Popen", return_value=process) as popen,
            mock.patch.object(runner, "wait_for_server"),
        ):
            result = runner.start_server(
                Path("/tmp/aeronet-bench-server"), 8090, 16, "http1",
                command_prefix=("taskset", "-c", "4-19"),
            )

        self.assertIs(result, process)
        command = popen.call_args.args[0]
        self.assertIn("--client-bench", command)
        self.assertEqual(command[:4], ["taskset", "-c", "4-19", "/tmp/aeronet-bench-server"])


if __name__ == "__main__":
    unittest.main()
