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
        completed = mock.Mock(
            stdout='{"rps":123.0}\n', stderr="", returncode=0,
        )
        with mock.patch.object(runner.subprocess, "run", return_value=completed) as run:
            result = runner.run_driver(
                Path("/tmp/curl-bench-client"), "http://127.0.0.1:8090", "small-get",
                8, "1s", "100ms", "http1", profiler,
                command_prefix=("taskset", "-c", "2-3"), sample=1, repeat=3,
            )

        self.assertEqual(result, {"rps": 123.0})
        self.assertEqual(profiler.parts, ["http1", "curl", "small-get", "sample-2"])
        command = run.call_args.args[0]
        self.assertEqual(command[:4], ["taskset", "-c", "2-3", "/tmp/curl-bench-client"])
        threads_index = command.index("--threads")
        self.assertEqual(command[threads_index + 1], "8")


class ConcurrencyTest(unittest.TestCase):
    def test_default_clients_stay_below_server_workers(self) -> None:
        self.assertEqual(runner._default_connections(4, 16), 12)
        self.assertEqual(runner._default_connections(6, 12), 11)
        self.assertEqual(runner._default_connections(1, 22), 3)

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
