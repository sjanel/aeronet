#!/usr/bin/env python3
"""Render static HTML charts from benchmark JSON files for GitHub Pages.

Supports both HTTP (wrk/h2load) and WebSocket (k6) benchmark formats.
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


def _is_websocket(summary: Dict[str, Any]) -> bool:
    return summary.get("benchmark_type") == "websocket"


# ------ Metric definitions per benchmark type ------------------------------ #

# Each metric is (key, data_key, filter_metric, chart_title, axis_label,
#                  scale_type, log_min, format_metric, table_label, lower_is_better)
_HTTP_METRICS = [
    ("rps", "rps", "rps", "Requests/sec (RPS)", "Requests/sec",
     "linear", 0, "rps", "Requests/sec", False),
    ("latency", "latency", "latency", "Avg Latency (ms)", "Latency (ms)",
     "logarithmic", 1e-4, "latency", "Latency", True),
    ("transfer", "transfer", "transfer", "Data transferred (MB)", "Transfer (MB)",
     "logarithmic", 1e-3, "transfer", "Throughput", False),
    ("memory_rss", "memory_rss", "memory", "Memory RSS (MB)", "RSS (MB) (log scale)",
     "logarithmic", 1e-2, "memory", "Memory", True),
    ("memory_peak", "memory_peak", "memory", "Memory Peak (MB)", "Peak (MB) (log scale)",
     "logarithmic", 1e-2, "memory", None, True),
]

_WS_METRICS = [
    ("rps", "rps", "rps", "Messages/sec", "Messages/sec",
     "linear", 0, "rps", "Messages/sec", False),
    ("latency", "latency", "latency", "Latency p95 (ms)", "Latency p95 (ms)",
     "logarithmic", 1e-4, "latency", "Latency p95", True),
]


def _get_metrics(summary: Dict[str, Any]) -> list:
    return _WS_METRICS if _is_websocket(summary) else _HTTP_METRICS


def _build_chart_payload(
    servers: List[str],
    scenarios: List[str],
    results: Dict[str, Any],
    metrics: list,
) -> Dict[str, Any]:
    """Build a JS-friendly chart payload with numeric values."""
    units = {"latency": "ms", "transfer": "MB"}

    # Collect all data_keys from metric definitions
    data_keys = [m[1] for m in metrics]

    payload: Dict[str, Any] = {
        "servers": servers,
        "scenarios": [],
        "units": units,
    }

    # Build chartDefs for JS chart initialization
    chart_defs = []
    for key, data_key, filter_metric, chart_title, axis_label, scale_type, log_min, format_metric, _tbl, _lib in metrics:
        canvas_id = f"chart-{key.replace('_', '-')}"
        chart_defs.append({
            "canvasId": canvas_id,
            "dataKey": data_key,
            "filterMetric": filter_metric,
            "chartTitle": chart_title,
            "axisLabel": axis_label,
            "scaleType": scale_type,
            "logMin": log_min,
            "formatMetric": format_metric,
        })
    payload["chartDefs"] = chart_defs

    for scenario in scenarios:
        scen = results.get(scenario, {})
        numeric: Dict[str, Any] = {"name": scenario}

        for _key, data_key, *_ in metrics:
            if data_key in ("memory_rss", "memory_peak"):
                vals = []
                for srv in servers:
                    mem_dict = scen.get("memory", {}).get(srv, {})
                    if isinstance(mem_dict, dict):
                        vals.append(mem_dict.get("rss_mb" if data_key == "memory_rss" else "peak_mb"))
                    else:
                        vals.append(None)
                numeric[data_key] = vals
            else:
                numeric[data_key] = [
                    _parse_metric_value(scen.get(data_key, {}).get(srv), data_key)
                    for srv in servers
                ]

        payload["scenarios"].append(numeric)
    return payload


def _build_chart_cards_html(metrics: list) -> str:
    """Generate HTML for chart cards based on available metrics."""
    parts = []
    for key, _data_key, filter_metric, chart_title, *_ in metrics:
        canvas_id = f"chart-{key.replace('_', '-')}"
        parts.append(f"""<div class="chart-card" data-metric="{_esc(filter_metric)}">
          <h3 style="margin:.25rem 0">{_esc(chart_title)}</h3>
          <div class="chart-shell"><canvas id="{canvas_id}"></canvas></div>
        </div>""")
    return "\n        ".join(parts)


def _conn_label(summary: Dict[str, Any]) -> str:
    """Human-readable label for a connection-count configuration."""
    conns = summary.get("connections")
    threads = summary.get("threads")
    protocol = summary.get("protocol", "http1")
    prefix = ""
    if protocol != "http1":
        prefix = f"[{protocol}] "
    if conns is not None and threads:
        per_thread = conns // threads
        return f"{prefix}{conns} total ({per_thread}/thread)"
    if conns is not None:
        return f"{prefix}{conns} total"
    return f"{prefix}default" if prefix else "default"


def render_html(summaries: List[Dict[str, Any]]) -> str:
    """Render HTML from one or more benchmark summaries.

    Each summary corresponds to a different run configuration (e.g. different
    connection counts).  A connection-count selector is shown when there are
    multiple summaries.  Supports both HTTP and WebSocket benchmark data.
    """
    first = summaries[0]
    threads = first.get("threads")
    duration = first.get("duration")
    warmup = first.get("warmup")
    protocol = first.get("protocol", "http1")
    tool = first.get("tool", "wrk")
    is_ws = _is_websocket(first)
    metrics = _get_metrics(first)

    # Build per-config payloads ------------------------------------------------
    configs: List[Dict[str, Any]] = []
    for summary in summaries:
        servers = summary.get("servers", [])
        scenarios = summary.get("scenarios", [])
        results = summary.get("results", {})
        label = _conn_label(summary)
        conns = summary.get("connections")

        # Build tables for each metric that has a table_label
        tables: Dict[str, str] = {}
        for key, _dkey, _fmetric, _ctitle, _alabel, _stype, _lmin, _fmetric2, table_label, lower_is_better in metrics:
            if table_label is not None:
                tables[key] = _build_table(
                    servers, scenarios, results, key,
                    lower_is_better=lower_is_better,
                )

        chart_payload = _build_chart_payload(servers, scenarios, results, metrics)

        scenario_options = ['<option value="all" selected>All scenarios</option>']
        scenario_options.extend(
            f'<option value="{_esc(s)}">{_esc(s)}</option>' for s in scenarios
        )

        configs.append({
            "label": label,
            "connections": conns,
            "tables": tables,
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

    # Build table tabs per config
    # Collect metrics that have table labels
    table_metrics = [
        (key, table_label)
        for key, _dkey, _fmetric, _ctitle, _alabel, _stype, _lmin, _fmetric2, table_label, _lib in metrics
        if table_label is not None
    ]

    table_tabs_parts = []
    for idx, cfg in enumerate(configs):
        display = "block" if idx == 0 else "none"
        first_metric_key = table_metrics[0][0] if table_metrics else "rps"

        tab_buttons = []
        tab_contents = []
        for mi, (mkey, mlabel) in enumerate(table_metrics):
            active = " active" if mi == 0 else ""
            tab_buttons.append(
                f'<button class="table-tab{active}" data-table="{mkey}">{_esc(mlabel)}</button>'
            )
            show = "block" if mi == 0 else "none"
            tab_contents.append(
                f'<div class="table-content" id="table-{mkey}-{idx}" '
                f'style="display: {show};">{cfg["tables"].get(mkey, "")}</div>'
            )

        table_tabs_parts.append(f"""
<div class="conn-config-tables" data-conn-idx="{idx}" style="display:{display}">
  <div class="table-tabs">
    <div class="tabs-nav">
      {''.join(tab_buttons)}
    </div>
    {''.join(tab_contents)}
  </div>
</div>
""")
    table_tabs_html = "\n".join(table_tabs_parts)

    # Meta cards
    if is_ws:
        vus = first.get("vus")
        meta_cards = f"""
<div class="meta-cards">
  <div><span>Protocol</span><strong>WebSocket</strong></div>
  <div><span>Tool</span><strong>{_esc(tool)}</strong></div>
  <div><span>Threads</span><strong>{_esc(str(threads))}</strong></div>
  <div><span>VUs</span><strong>{_esc(str(vus))}</strong></div>
  <div><span>Duration</span><strong>{_esc(str(duration))}</strong></div>
  <div><span>Warmup</span><strong>{_esc(str(warmup))}</strong></div>
</div>
"""
    else:
        h2_streams = first.get("h2_streams")
        protocol_display = protocol.upper().replace("H2C", "H2C (cleartext)").replace("H2-TLS", "H2 over TLS")
        meta_cards = f"""
<div class="meta-cards">
  <div><span>Protocol</span><strong>{_esc(protocol_display)}</strong></div>
  <div><span>Tool</span><strong>{_esc(tool)}</strong></div>
  <div><span>Threads</span><strong>{_esc(str(threads))}</strong></div>
  <div><span>{_esc(tool)} duration</span><strong>{_esc(str(duration))}</strong></div>
  <div><span>{_esc(tool)} warmup</span><strong>{_esc(str(warmup))}</strong></div>"""
        if h2_streams is not None:
            meta_cards += f"""
  <div><span>H2 Streams/conn</span><strong>{h2_streams}</strong></div>"""
        meta_cards += """
</div>
"""

    scenario_options_html = configs[0]["scenario_options_html"]

    # Collect unique filter_metric values for metric filter options
    seen_filter_metrics: List[str] = []
    metric_labels_list = [("all", "All metrics")]
    for _key, _dkey, filter_metric, chart_title, *_ in metrics:
        if filter_metric not in seen_filter_metrics:
            seen_filter_metrics.append(filter_metric)
            metric_labels_list.append((filter_metric, chart_title))

    metric_options_html = "\n".join(
        f'<option value="{key}"{" selected" if key == "rps" else ""}>{_esc(label)}</option>'
        for key, label in metric_labels_list
    )

    # Build chart cards HTML
    chart_cards_html = _build_chart_cards_html(metrics)

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

    # Page title
    page_title = "WebSocket Benchmarks" if is_ws else "aeronet Benchmarks"

    html_out = (
        tpl.replace("__TABLE_TABS_HTML__", table_tabs_html)
        .replace("__PAYLOAD_JSON__", configs_js)
        .replace("__META_CARDS__", meta_cards)
        .replace("__SCENARIO_OPTIONS__", scenario_options_html)
        .replace("__METRIC_OPTIONS__", metric_options_html)
        .replace("__CONN_SELECTOR__", conn_selector_html)
        .replace("__CHART_CARDS_HTML__", chart_cards_html)
        .replace(">aeronet Benchmarks <", f">{_esc(page_title)} <")
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
