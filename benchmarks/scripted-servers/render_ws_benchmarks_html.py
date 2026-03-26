#!/usr/bin/env python3
"""Render a static HTML report for WebSocket benchmark JSON output."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Dict, List, Optional


def _esc(text: str) -> str:
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )


def load_summary(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as fp:
        return json.load(fp)


def _metric_rate(metrics: Dict[str, Any], key: str) -> Optional[float]:
    value = metrics.get(key)
    if isinstance(value, dict):
        rate = value.get("rate")
        if isinstance(rate, (int, float)):
            return float(rate)
    return None


def _trend_p95(metrics: Dict[str, Any], key: str) -> Optional[float]:
    value = metrics.get(key)
    if isinstance(value, dict):
        p95 = value.get("p95")
        if isinstance(p95, (int, float)):
            return float(p95)
    return None


def _primary_rate(metrics: Dict[str, Any]) -> Optional[float]:
    for key in (
        "ws_messages_sent",
        "ws_msgs_sent",
        "ws_messages_received",
        "ws_msgs_received",
        "ws_sessions",
    ):
        rate = _metric_rate(metrics, key)
        if rate is not None:
            return rate
    return None


def _primary_p95(metrics: Dict[str, Any]) -> Optional[float]:
    for key in (
        "ws_echo_rtt_ms",
        "ws_ping_rtt_ms",
        "ws_connection_lifetime_ms",
        "ws_connecting",
        "ws_session_duration",
    ):
        p95 = _trend_p95(metrics, key)
        if p95 is not None:
            return p95
    return None


def _extract(summary: Dict[str, Any]) -> Dict[str, Any]:
    rows = summary.get("results", [])
    scenarios: List[str] = []
    servers = summary.get("config", {}).get("servers", [])
    table: Dict[str, Dict[str, Dict[str, Any]]] = {}

    for row in rows:
        if row.get("tool") != "k6":
            continue
        scenario = str(row.get("scenario", "unknown"))
        server = str(row.get("server", "unknown"))
        metrics = row.get("metrics", {}) if isinstance(row.get("metrics"), dict) else {}
        success = bool(row.get("success", False))
        if scenario not in scenarios:
            scenarios.append(scenario)
        table.setdefault(scenario, {})[server] = {
            "success": success,
            "p95_ms": _primary_p95(metrics),
            "rate": _primary_rate(metrics),
            "check_fails": (
                int(metrics.get("checks", {}).get("fails", 0))
                if isinstance(metrics.get("checks"), dict)
                else 0
            ),
            "data_sent_bps": _metric_rate(metrics, "data_sent"),
            "data_recv_bps": _metric_rate(metrics, "data_received"),
        }

    return {
        "timestamp": summary.get("timestamp", ""),
        "config": summary.get("config", {}),
        "scenarios": scenarios,
        "servers": servers,
        "table": table,
    }


def _render_rows(data: Dict[str, Any]) -> str:
    scenarios = data["scenarios"]
    servers = data["servers"]
    table = data["table"]

    rows: List[str] = []
    for scenario in scenarios:
        row = [f"<td>{_esc(scenario)}</td>"]
        for server in servers:
            cell = table.get(scenario, {}).get(server)
            if not cell or not cell.get("success"):
                row.append("<td class='na'>-</td>")
                continue
            p95 = cell.get("p95_ms")
            rate = cell.get("rate")
            fails = cell.get("check_fails", 0)
            parts: List[str] = []
            if isinstance(p95, (int, float)):
                parts.append(f"p95={p95:.3f}ms")
            if isinstance(rate, (int, float)):
                parts.append(f"{rate:,.0f}/s")
            if fails:
                parts.append(f"fails={fails}")
            row.append(f"<td>{_esc(', '.join(parts) if parts else 'OK')}</td>")
        rows.append("<tr>" + "".join(row) + "</tr>")

    return "\n".join(rows)


def _chart_payload(data: Dict[str, Any]) -> str:
    scenarios = data["scenarios"]
    servers = data["servers"]
    table = data["table"]
    payload = {
        "servers": servers,
        "scenarios": scenarios,
        "p95": [
            [
                table.get(sc, {}).get(srv, {}).get("p95_ms")
                for srv in servers
            ]
            for sc in scenarios
        ],
        "rate": [
            [
                table.get(sc, {}).get(srv, {}).get("rate")
                for srv in servers
            ]
            for sc in scenarios
        ],
    }
    return json.dumps(payload).replace("</", "<\\/")


def render_html(summary: Dict[str, Any]) -> str:
    data = _extract(summary)
    cfg = data["config"]
    servers = data["servers"]

    headers = "".join(f"<th>{_esc(s)}</th>" for s in servers)
    rows = _render_rows(data)
    payload = _chart_payload(data)

    return f"""<!DOCTYPE html>
<html lang=\"en\">
<head>
  <meta charset=\"utf-8\" />
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />
  <title>WebSocket Benchmark Report</title>
  <style>
    :root {{
      --bg:#0d1117;
      --panel:#161b22;
      --muted:#8b949e;
      --text:#c9d1d9;
      --accent:#58a6ff;
      --good:#3fb950;
      --warn:#d29922;
      --bad:#f85149;
      --line:#30363d;
    }}
    body {{font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, sans-serif; background:var(--bg); color:var(--text); margin:0;}}
    .wrap {{max-width:1200px; margin:28px auto; padding:0 18px;}}
    h1 {{margin:0 0 14px 0; font-size:1.55rem;}}
    .meta {{display:grid; grid-template-columns: repeat(auto-fit,minmax(180px,1fr)); gap:10px; margin-bottom:16px;}}
    .card {{background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:10px 12px;}}
    .card span {{display:block; color:var(--muted); font-size:0.85rem;}}
    .card strong {{display:block; margin-top:4px; font-size:1rem;}}
    .section {{background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:14px; margin-top:14px;}}
    .controls {{display:flex; gap:10px; flex-wrap:wrap; margin-bottom:12px;}}
    select {{background:#0b1220; color:var(--text); border:1px solid var(--line); border-radius:8px; padding:6px 8px;}}
    table {{width:100%; border-collapse:collapse;}}
    th, td {{padding:8px 10px; border-bottom:1px solid var(--line); text-align:left; font-size:0.92rem;}}
    th {{color:#9fb3c8;}}
    td.na {{color:var(--muted);}}
    .chart {{height:320px; position:relative;}}
    .legend {{display:flex; gap:14px; flex-wrap:wrap; margin:8px 0 0 0; color:var(--muted); font-size:0.85rem;}}
    .dot {{display:inline-block; width:10px; height:10px; border-radius:999px; margin-right:6px;}}
  </style>
</head>
<body>
<div class=\"wrap\">
  <h1>WebSocket Benchmarks</h1>
  <div class=\"meta\">
    <div class=\"card\"><span>Timestamp</span><strong>{_esc(str(data['timestamp']))}</strong></div>
    <div class=\"card\"><span>Servers</span><strong>{_esc(', '.join(servers))}</strong></div>
    <div class=\"card\"><span>VUs</span><strong>{_esc(str(cfg.get('vus', '-')))}</strong></div>
    <div class=\"card\"><span>Duration</span><strong>{_esc(str(cfg.get('duration', '-')))}</strong></div>
  </div>

  <div class=\"section\">
    <div class=\"controls\">
      <label>Metric
        <select id=\"metric\">
          <option value=\"rate\">Throughput (/s)</option>
          <option value=\"p95\">Latency p95 (ms)</option>
        </select>
      </label>
      <label>Scenario
        <select id=\"scenario\">
          <option value=\"all\">All scenarios</option>
          {''.join(f'<option value="{_esc(s)}">{_esc(s)}</option>' for s in data['scenarios'])}
        </select>
      </label>
    </div>
    <div class=\"chart\" id=\"chart\"></div>
    <div class=\"legend\" id=\"legend\"></div>
  </div>

  <div class=\"section\">
    <h3 style=\"margin-top:0\">Per-scenario summary</h3>
    <table>
      <thead><tr><th>Scenario</th>{headers}</tr></thead>
      <tbody>{rows}</tbody>
    </table>
  </div>
</div>
<script>
const payload = {payload};
const colors = ["#58a6ff", "#3fb950", "#d29922", "#f85149", "#a371f7", "#ff7b72"];

function aggregateByServer(matrix, scenarios) {{
  const out = new Array(payload.servers.length).fill(null).map(() => []);
  scenarios.forEach((sc) => {{
    const idx = payload.scenarios.indexOf(sc);
    if (idx < 0) return;
    matrix[idx].forEach((v, sidx) => {{
      if (typeof v === 'number') out[sidx].push(v);
    }});
  }});
  return out.map(arr => arr.length ? arr.reduce((a,b)=>a+b,0)/arr.length : null);
}}

function barChart(values, labels, title, lowerIsBetter) {{
  const root = document.getElementById('chart');
  root.innerHTML = '';
  const valid = values.filter(v => typeof v === 'number' && isFinite(v));
  const max = valid.length ? Math.max(...valid) : 1;
  const min = valid.length ? Math.min(...valid) : 0;
  const width = root.clientWidth || 1000;
  const height = root.clientHeight || 320;
  const pad = {{l:60, r:20, t:30, b:40}};
  const innerW = width - pad.l - pad.r;
  const innerH = height - pad.t - pad.b;
  const barW = Math.max(30, innerW / Math.max(1, labels.length * 1.7));
  const gap = barW * 0.7;

  let svg = `<svg width="${{width}}" height="${{height}}" viewBox="0 0 ${{width}} ${{height}}">`;
  svg += `<text x="${{pad.l}}" y="18" fill="#8b949e" font-size="13">${{title}}</text>`;
  svg += `<line x1="${{pad.l}}" y1="${{pad.t+innerH}}" x2="${{pad.l+innerW}}" y2="${{pad.t+innerH}}" stroke="#30363d"/>`;

  labels.forEach((label, i) => {{
    const value = values[i];
    const x = pad.l + i * (barW + gap) + gap/2;
    if (typeof value === 'number' && isFinite(value)) {{
      const h = max > 0 ? (value / max) * innerH : 0;
      const y = pad.t + innerH - h;
      const best = lowerIsBetter ? (value === min) : (value === max);
      const stroke = best ? '#ffffff' : 'none';
      svg += `<rect x="${{x}}" y="${{y}}" width="${{barW}}" height="${{Math.max(1, h)}}" fill="${{colors[i % colors.length]}}" stroke="${{stroke}}"/>`;
      svg += `<text x="${{x + barW/2}}" y="${{y - 6}}" text-anchor="middle" fill="#c9d1d9" font-size="12">${{value.toFixed(3)}}</text>`;
    }} else {{
      svg += `<text x="${{x + barW/2}}" y="${{pad.t + innerH - 6}}" text-anchor="middle" fill="#8b949e" font-size="12">n/a</text>`;
    }}
    svg += `<text x="${{x + barW/2}}" y="${{pad.t + innerH + 16}}" text-anchor="middle" fill="#8b949e" font-size="12">${{label}}</text>`;
  }});

  svg += `</svg>`;
  root.innerHTML = svg;

  const legend = document.getElementById('legend');
  legend.innerHTML = labels.map((label, i) =>
    `<span><span class="dot" style="background:${{colors[i % colors.length]}}"></span>${{label}}</span>`
  ).join('');
}}

function render() {{
  const metric = document.getElementById('metric').value;
  const selected = document.getElementById('scenario').value;
  const scenarios = selected === 'all' ? payload.scenarios : [selected];
  if (metric === 'rate') {{
    const values = aggregateByServer(payload.rate, scenarios);
    barChart(values, payload.servers, 'Average throughput across selected scenarios (/s)', false);
  }} else {{
    const values = aggregateByServer(payload.p95, scenarios);
    barChart(values, payload.servers, 'Average p95 latency across selected scenarios (ms)', true);
  }}
}}

document.getElementById('metric').addEventListener('change', render);
document.getElementById('scenario').addEventListener('change', render);
render();
</script>
</body>
</html>
"""


def main() -> None:
    parser = argparse.ArgumentParser(description="Render HTML report for WS benchmark JSON")
    parser.add_argument("--input", type=Path, required=True, help="Input ws_benchmark_*.json")
    parser.add_argument("--output", type=Path, default=Path("ws_benchmark.html"))
    args = parser.parse_args()

    summary = load_summary(args.input)
    html = render_html(summary)
    args.output.write_text(html, encoding="utf-8")


if __name__ == "__main__":
    main()
