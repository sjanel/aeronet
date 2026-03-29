#!/usr/bin/env python3
"""Shared utilities for HTTP and WebSocket benchmark scripts."""
from __future__ import annotations

from typing import Any, Dict, List, Optional, Tuple


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
            if any(cleaned.endswith(unit) for unit in ("us", "ms", "s")):
                suffix = "".join(ch for ch in cleaned if not ch.isdigit() and ch != ".")
                number = float("".join(ch for ch in cleaned if ch.isdigit() or ch == "."))
                scale = {"us": 1, "ms": 1000, "s": 1_000_000}.get(suffix, 1)
                return number * scale
            if any(cleaned.endswith(unit) for unit in ("KB", "MB", "GB")):
                suffix = "".join(ch for ch in cleaned if not ch.isdigit() and ch != ".")
                number = float("".join(ch for ch in cleaned if ch.isdigit() or ch == "."))
                scale = {"B": 1, "KB": 1024, "MB": 1_048_576, "GB": 1_073_741_824}.get(
                    suffix, 1
                )
                return number * scale
            return float(cleaned)
        except ValueError:
            return None
