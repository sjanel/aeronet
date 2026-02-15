#!/usr/bin/env python3
"""Render static HTML charts from benchmark JSON files for GitHub Pages.

Supports multiple input files (e.g. different connection-count runs) and
renders tabs to switch between them.  Falls back gracefully to a single
run when only one file is provided.

This script is intentionally dependency-light: it emits a self-contained
HTML file with a simple table-based summary and minimal inline JS for
interactive sorting.
"""
from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any, Dict, List, Optional


def load_summary(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as fp:
        return json.load(fp)


def _parse_metric_value(value: Any, metric: str) -> Optional[float]:
    """Parse string metric values to float for comparison."""
    if value is None:
        return None
    if isinstance(value, (int, float)):
        val = float(value)
    else:
        text = str(value).strip()
        if not text or text == "-":
            return None
        match = re.match(r"^([-+]?\d[\d,\.eE+-]*)(.*)$", text)
        if match:
            number_part = match.group(1).replace(",", "")
            unit_part = match.group(2).strip().lower()
        else:
            return None
        try:
            val = float(number_part)
        except ValueError:
            return None

        if metric == "latency":
            if unit_part.startswith("ms"):
                return val
            if unit_part.startswith(("us", "\u00b5s", "\u03bcs")):
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

        return val
    return val


def _esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def _build_table(
    servers: List[str],
    scenarios: List[str],
    results: Dict[str, Any],
    metric_key: str,
    lower_is_better: bool = False,
) -> str:
    """Build an HTML table for a specific metric."""
    rows = []
    for scenario in scenarios:
        scen_data = results.get(scenario, {})
        if metric_key == "memory":
            values = scen_data.get("memory", {})
        else:
            values = scen_data.get(metric_key, {})

        row_cells = [f"<td>{_esc(scenario)}</td>"]
        best_val = None
        best_server = None
        server_values: Dict[str, float] = {}

        for srv in servers:
            if metric_key == "memory":
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
                    parsed = _parse_metric_value(val, metric_key)
                    if parsed is not None:
                        server_values[srv] = parsed

            row_cells.append(f"<td data-server='{_esc(srv)}'>{_esc(cell)}</td>")

        for srv, parsed_val in server_values.items():
            if best_val is None:
                best_val = parsed_val
                best_server = srv
            elif lower_is_better and parsed_val < best_val:
                best_val = parsed_val
                best_server = srv
            elif not lower_is_better and parsed_val > best_val:
                best_val = parsed_val
                best_server = srv

        row_cells.append(f"<td class='best-cell'>{_esc(best_server or '-')}</td>")
        rows.append("<tr>" + "".join(row_cells) + "</tr>")

    header_cells = ["Scenario"] + servers + ["Best"]
    header_html = "".join(f"<th>{_esc(h)}</th>" for h in header_cells)
    return f"""
<table class="benchmark-table">
  <thead><tr>{header_html}</tr></thead>
  <tbody>{''.join(rows)}</tbody>
</table>
"""


def _build_chart_payload(
    servers: List[str],
    scenarios: List[str],
    results: Dict[str, Any],
) -> Dict[str, Any]:
    """Build a JS-friendly chart payload with numeric values."""
    units = {"latency": "ms", "transfer": "MB"}
    payload: Dict[str, Any] = {
        "servers": servers,
        "scenarios": [],
        "units": units,
    }
    for scenario in scenarios:
        scen = results.get(scenario, {})
        numeric: Dict[str, Any] = {
            "name": scenario,
            "rps": [],
            "latency": [],
            "transfer": [],
            "memory_rss": [],
            "memory_peak": [],
        }
        for srv in servers:
            numeric["rps"].append(
                _parse_metric_value(scen.get("rps", {}).get(srv), "rps")
            )
            numeric["latency"].append(
                _parse_metric_value(scen.get("latency", {}).get(srv), "latency")
            )
            numeric["transfer"].append(
                _parse_metric_value(scen.get("transfer", {}).get(srv), "transfer")
            )
            mem_dict = scen.get("memory", {}).get(srv, {})
            if isinstance(mem_dict, dict):
                numeric["memory_rss"].append(mem_dict.get("rss_mb"))
                numeric["memory_peak"].append(mem_dict.get("peak_mb"))
            else:
                numeric["memory_rss"].append(None)
                numeric["memory_peak"].append(None)
        payload["scenarios"].append(numeric)
    return payload


def _conn_label(summary: Dict[str, Any]) -> str:
    """Human-readable label for a connection-count configuration."""
    conns = summary.get("connections")
    threads = summary.get("threads")
    if conns is not None and threads:
        per_thread = conns // threads
        return f"{conns} total ({per_thread}/thread)"
    if conns is not None:
        return f"{conns} total"
    return "default"


def render_html(summaries: List[Dict[str, Any]]) -> str:
    """Render HTML from one or more benchmark summaries.

    Each summary corresponds to a different run configuration (e.g. different
    connection counts).  A connection-count selector is shown when there are
    multiple summaries.
    """
    first = summaries[0]
    threads = first.get("threads")
    duration = first.get("duration")
    warmup = first.get("warmup")

    # Build per-config payloads ------------------------------------------------
    configs: List[Dict[str, Any]] = []
    for summary in summaries:
        servers = summary.get("servers", [])
        scenarios = summary.get("scenarios", [])
        results = summary.get("results", {})
        label = _conn_label(summary)
        conns = summary.get("connections")

        rps_table = _build_table(servers, scenarios, results, "rps", lower_is_better=False)
        latency_table = _build_table(servers, scenarios, results, "latency", lower_is_better=True)
        transfer_table = _build_table(servers, scenarios, results, "transfer", lower_is_better=False)
        memory_table = _build_table(servers, scenarios, results, "memory", lower_is_better=True)

        chart_payload = _build_chart_payload(servers, scenarios, results)

        scenario_options = ['<option value="all" selected>All scenarios</option>']
        scenario_options.extend(
            f'<option value="{_esc(s)}">{_esc(s)}</option>' for s in scenarios
        )

        configs.append({
            "label": label,
            "connections": conns,
            "rps_table": rps_table,
            "latency_table": latency_table,
            "transfer_table": transfer_table,
            "memory_table": memory_table,
            "chart_payload": chart_payload,
            "scenario_options_html": "\n".join(scenario_options),
        })

    # Build connection selector (shown only when > 1 config)
    has_multi = len(configs) > 1
    conn_selector_html = ""
    if has_multi:
        conn_opts = "\n".join(
            f'<option value="{i}">{_esc(cfg["label"])}</option>'
            for i, cfg in enumerate(configs)
        )
        conn_selector_html = f"""
<label>Connections
  <select id="conn-filter">{conn_opts}</select>
</label>
"""

    # Build table tabs per config (hidden via JS)
    table_tabs_parts = []
    for idx, cfg in enumerate(configs):
        display = "block" if idx == 0 else "none"
        table_tabs_parts.append(f"""
<div class="conn-config-tables" data-conn-idx="{idx}" style="display:{display}">
  <div class="table-tabs">
    <div class="tabs-nav">
      <button class="table-tab active" data-table="rps">Requests/sec</button>
      <button class="table-tab" data-table="latency">Latency</button>
      <button class="table-tab" data-table="throughput">Throughput</button>
      <button class="table-tab" data-table="memory">Memory</button>
    </div>
    <div class="table-content" id="table-rps-{idx}" style="display: block;">{cfg["rps_table"]}</div>
    <div class="table-content" id="table-latency-{idx}" style="display: none;">{cfg["latency_table"]}</div>
    <div class="table-content" id="table-throughput-{idx}" style="display: none;">{cfg["transfer_table"]}</div>
    <div class="table-content" id="table-memory-{idx}" style="display: none;">{cfg["memory_table"]}</div>
  </div>
</div>
""")
    table_tabs_html = "\n".join(table_tabs_parts)

    meta_cards = f"""
<div class="meta-cards">
  <div><span>Threads</span><strong>{_esc(str(threads))}</strong></div>
  <div><span>wrk duration</span><strong>{_esc(str(duration))}</strong></div>
  <div><span>wrk warmup</span><strong>{_esc(str(warmup))}</strong></div>
</div>
"""

    scenario_options_html = configs[0]["scenario_options_html"]

    metric_labels = [
        ("all", "All metrics"),
        ("rps", "Requests/sec (RPS)"),
        ("latency", "Avg Latency (ms)"),
        ("transfer", "Data transferred (MB)"),
        ("memory", "Memory Usage (RSS & Peak)"),
    ]
    metric_options_html = "\n".join(
        f'<option value="{key}"{" selected" if key == "rps" else ""}>{label}</option>'
        for key, label in metric_labels
    )

    # Build the JS configs array
    configs_json_list = []
    for cfg in configs:
        entry = json.dumps(cfg["chart_payload"]).replace("</", "<\\/")
        configs_json_list.append(entry)
    configs_js = "[" + ",".join(configs_json_list) + "]"

    # Load template
    tpl_path = Path(__file__).parent / "template" / "benchmarks_template.html"
    if not tpl_path.exists():
        raise FileNotFoundError(f"Template file not found: {tpl_path}")
    tpl = tpl_path.read_text(encoding="utf-8")

    html_out = (
        tpl.replace("__TABLE_TABS_HTML__", table_tabs_html)
        .replace("__PAYLOAD_JSON__", configs_js)
        .replace("__META_CARDS__", meta_cards)
        .replace("__SCENARIO_OPTIONS__", scenario_options_html)
        .replace("__METRIC_OPTIONS__", metric_options_html)
        .replace("__CONN_SELECTOR__", conn_selector_html)
    )
    return html_out


def main() -> None:
    import argparse

    parser = argparse.ArgumentParser(
        description="Render HTML from benchmark JSON files (supports multiple inputs)"
    )
    parser.add_argument(
        "--input",
        type=Path,
        nargs="+",
        action="append",
        default=None,
        help="One or more benchmark JSON files (e.g. different connection counts)",
    )
    parser.add_argument("--output", type=Path, default=Path("benchmarks.html"))
    args = parser.parse_args()

    input_groups = args.input or [[Path("benchmark_latest.json")]]
    input_files = [path for group in input_groups for path in group]

    summaries = [load_summary(path) for path in input_files]

    # Sort by connection count so the selector order is deterministic
    summaries.sort(key=lambda s: s.get("connections", 0) or 0)

    html = render_html(summaries)
    args.output.write_text(html, encoding="utf-8")


if __name__ == "__main__":
    main()
