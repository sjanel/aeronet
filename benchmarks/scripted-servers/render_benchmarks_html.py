#!/usr/bin/env python3
"""Render static HTML charts from benchmark_latest.json for GitHub Pages.

This script is intentionally dependency-light: it emits a self-contained
HTML file with a simple table-based summary and minimal inline JS for
interactive sorting.
"""
from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any, Dict, Optional


def load_summary(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as fp:
        return json.load(fp)


def render_html(summary: Dict[str, Any]) -> str:
    servers = summary.get("servers", [])
    scenarios = summary.get("scenarios", [])
    results = summary.get("results", {})
    threads = summary.get("threads")
    duration = summary.get("duration")
    warmup = summary.get("warmup")

    def esc(s: str) -> str:
        return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

    def parse_metric_value(value: Any, metric: str) -> Optional[float]:
        """Parse string metric values to float for comparison."""
        if value is None:
            return None
        if isinstance(value, (int, float)):
            val = float(value)
        else:
            text = str(value).strip()
            if not text or text == "-":
                return None
            number_part = text
            unit_part = ""
            match = re.match(r"^([-+]?\d[\d,\.eE+-]*)(.*)$", text)
            if match:
                number_part = match.group(1)
                unit_part = match.group(2).strip().lower()
            number_part = number_part.replace(",", "")
            try:
                val = float(number_part)
            except ValueError:
                return None

            if metric == "latency":
                if unit_part.startswith("ms"):
                    return val
                if (
                    unit_part.startswith("us")
                    or unit_part.startswith("µs")
                    or unit_part.startswith("μs")
                ):
                    return val / 1000.0
                if unit_part.startswith("ns"):
                    return val / 1_000_000.0
                if unit_part.startswith("s"):
                    return val * 1000.0
                return val if abs(val) > 0.05 else val * 1000.0

            if metric == "transfer":
                if "gb" in unit_part:
                    return val * 1024.0
                if "mb" in unit_part:
                    return val
                if "kb" in unit_part:
                    return val / 1024.0
                if unit_part.endswith("b"):
                    return val / (1024.0 * 1024.0)
                return val

            # Default (RPS)
            return val

        return val

    def build_table(
        metric_key: str, metric_label: str, lower_is_better: bool = False
    ) -> str:
        """Build a table for a specific metric (rps, latency, transfer, memory)."""
        rows = []
        for scenario in scenarios:
            scen_data = results.get(scenario, {})
            if metric_key == "memory":
                values = scen_data.get("memory", {})
            else:
                values = scen_data.get(metric_key, {})

            row_cells = [f"<td>{esc(scenario)}</td>"]
            best_val = None
            best_server = None
            server_values = {}  # Track parsed values for comparison

            for srv in servers:
                if metric_key == "memory":
                    # values is {srv: {rss_mb, peak_mb, ...}} for memory
                    mem_dict = values.get(srv, {})
                    if isinstance(mem_dict, dict):
                        rss = mem_dict.get("rss_mb")
                        if isinstance(rss, (int, float)):
                            cell = f"{rss:.1f}MB"
                            server_values[srv] = float(rss)
                        else:
                            cell = "-"
                    else:
                        cell = "-"
                else:
                    val = values.get(srv)
                    cell = "-"
                    if isinstance(val, (int, float, str)):
                        cell = str(val)
                        # Parse numeric value for comparison
                        parsed = parse_metric_value(val, metric_key)
                        if parsed is not None:
                            server_values[srv] = parsed

                row_cells.append(f"<td data-server='{esc(srv)}'>{esc(cell)}</td>")

            # Determine best server based on metric type
            for srv, parsed_val in server_values.items():
                if best_val is None:
                    best_val = parsed_val
                    best_server = srv
                else:
                    if lower_is_better:
                        if parsed_val < best_val:
                            best_val = parsed_val
                            best_server = srv
                    else:
                        if parsed_val > best_val:
                            best_val = parsed_val
                            best_server = srv

            row_cells.append(f"<td class='best-cell'>{esc(best_server or '-')}</td>")
            rows.append("<tr>" + "".join(row_cells) + "</tr>")

        header_cells = ["Scenario"] + servers + ["Best"]
        header_html = "".join(f"<th>{esc(h)}</th>" for h in header_cells)
        table_html = f"""
<table class="benchmark-table">
  <thead>
    <tr>{header_html}</tr>
  </thead>
  <tbody>
    {''.join(rows)}
  </tbody>
</table>
"""
        return table_html

    rps_table = build_table(
        "rps", "Requests/sec (higher is better)", lower_is_better=False
    )
    latency_table = build_table(
        "latency", "Avg latency (lower is better)", lower_is_better=True
    )
    transfer_table = build_table(
        "transfer", "Throughput (higher is better)", lower_is_better=False
    )
    memory_table = build_table("memory", "Memory Usage", lower_is_better=True)

    table_tabs_html = f"""
<div class="table-tabs">
  <div class="tabs-nav">
    <button class="table-tab active" data-table="rps">Requests/sec</button>
    <button class="table-tab" data-table="latency">Latency</button>
    <button class="table-tab" data-table="throughput">Throughput</button>
    <button class="table-tab" data-table="memory">Memory</button>
  </div>
  <div class="table-content" id="table-rps" style="display: block;">{rps_table}</div>
  <div class="table-content" id="table-latency" hidden>{latency_table}</div>
  <div class="table-content" id="table-throughput" hidden>{transfer_table}</div>
  <div class="table-content" id="table-memory" hidden>{memory_table}</div>
</div>
"""

    meta_cards = f"""
<div class=\"meta-cards\">
  <div><span>Threads</span><strong>{esc(str(threads))}</strong></div>
  <div><span>wrk duration</span><strong>{esc(str(duration))}</strong></div>
  <div><span>wrk warmup</span><strong>{esc(str(warmup))}</strong></div>
</div>
"""

    scenario_options = ['<option value="all" selected>All scenarios</option>']
    scenario_options.extend(
        f'<option value="{esc(s)}">{esc(s)}</option>' for s in scenarios
    )
    scenario_options_html = "\n".join(scenario_options)

    metric_labels = [
        ("all", "All metrics"),
        ("rps", "Requests/sec (RPS)"),
        ("latency", "Avg Latency (ms)"),
        ("transfer", "Data transferred (MB)"),
        ("memory", "Memory Usage (RSS & Peak)"),
    ]
    metric_options_html = "\n".join(
        f"<option value=\"{key}\"{' selected' if key == 'rps' else ''}>{label}</option>"
        for key, label in metric_labels
    )

    units = {"latency": "ms", "transfer": "MB"}

    # Build a JS-friendly version of the summary data for charts with numeric values.
    chart_payload = {
        "servers": servers,
        "scenarios": [],
        "units": units,
    }
    for scenario in scenarios:
        scen = results.get(scenario, {})
        numeric = {
            "name": scenario,
            "rps": [],
            "latency": [],
            "transfer": [],
            "memory_rss": [],
            "memory_peak": [],
        }
        for srv in servers:
            numeric["rps"].append(
                parse_metric_value(scen.get("rps", {}).get(srv), "rps")
            )
            numeric["latency"].append(
                parse_metric_value(scen.get("latency", {}).get(srv), "latency")
            )
            numeric["transfer"].append(
                parse_metric_value(scen.get("transfer", {}).get(srv), "transfer")
            )
            mem_dict = scen.get("memory", {}).get(srv, {})
            if isinstance(mem_dict, dict):
                numeric["memory_rss"].append(mem_dict.get("rss_mb"))
                numeric["memory_peak"].append(mem_dict.get("peak_mb"))
            else:
                numeric["memory_rss"].append(None)
                numeric["memory_peak"].append(None)
        chart_payload["scenarios"].append(numeric)

    # Inline the JSON payload directly as a JS object literal. Escape closing
    # </script> sequences to avoid breaking out of the script tag.
    payload_json = json.dumps(chart_payload).replace("</", "<\\/")

    # Prefer loading an external template file if available to keep the
    # Python file small. Fall back to the embedded `template` string above.
    tpl_path = Path(__file__).parent / "template" / "benchmarks_template.html"
    if tpl_path.exists():
        tpl = tpl_path.read_text(encoding="utf-8")
    else:
        raise FileNotFoundError(f"Template file not found: {tpl_path}")

    # Fill placeholders safely. `payload_json` already escaped </script> sequences.
    html_out = (
        tpl.replace("__TABLE_TABS_HTML__", table_tabs_html)
        .replace("__PAYLOAD_JSON__", payload_json)
        .replace("__META_CARDS__", meta_cards)
        .replace("__SCENARIO_OPTIONS__", scenario_options_html)
        .replace("__METRIC_OPTIONS__", metric_options_html)
    )
    return html_out


def main() -> None:
    import argparse

    parser = argparse.ArgumentParser(
        description="Render HTML from benchmark_latest.json"
    )
    parser.add_argument("--input", type=Path, default=Path("benchmark_latest.json"))
    parser.add_argument("--output", type=Path, default=Path("benchmarks.html"))
    args = parser.parse_args()

    summary = load_summary(args.input)
    html = render_html(summary)
    args.output.write_text(html, encoding="utf-8")


if __name__ == "__main__":
    main()
