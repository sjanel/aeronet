#!/usr/bin/env python3
"""Focused tests for the scripted-server benchmark orchestrator."""
from __future__ import annotations

import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("run_benchmarks.py")
SPEC = importlib.util.spec_from_file_location("run_benchmarks", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)


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


if __name__ == "__main__":
    unittest.main()
