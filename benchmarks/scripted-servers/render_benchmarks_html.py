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
    return (s.replace("&", "&amp;")
             .replace("<", "&lt;")
             .replace(">", "&gt;"))

  rows_html = []
  for scenario in scenarios:
    scen_data = results.get(scenario, {})
    rps = scen_data.get("rps", {})
    latency = scen_data.get("latency", {})
    transfer = scen_data.get("transfer", {})
    winner = scen_data.get("winners", {}).get("rps", "-")
    for category, values in (("rps", rps), ("latency", latency), ("transfer", transfer)):
      label = {
          "rps": "Requests/sec (higher is better)",
          "latency": "Avg latency (lower is better)",
          "transfer": "Throughput (higher is better)",
      }[category]
      row_cells = [f"<td>{esc(scenario)}</td>", f"<td>{esc(label)}</td>"]
      best_val = None
      best_server = None
      for srv in servers:
        val = values.get(srv)
        cell = "-"
        if isinstance(val, (int, float, str)):
          cell = str(val)
        row_cells.append(f"<td data-server='{esc(srv)}'>{esc(cell)}</td>")
        if category == "rps" and isinstance(val, (int, float)):
          if best_val is None or val > best_val:
            best_val = val
            best_server = srv
      row_cells.append(f"<td>{esc(best_server or winner or '-')}</td>")
      rows_html.append("<tr>" + "".join(row_cells) + "</tr>")

  table_html = f"""
<table id=\"benchmarks\">
  <thead>
    <tr>
      <th>Scenario</th>
      <th>Metric</th>
      {''.join(f'<th>{esc(s)}</th>' for s in servers)}
      <th>Best (RPS)</th>
    </tr>
  </thead>
  <tbody>
    {''.join(rows_html)}
  </tbody>
</table>
"""

  meta_cards = f"""
<div class=\"meta-cards\">
  <div><span>Threads</span><strong>{esc(str(threads))}</strong></div>
  <div><span>wrk duration</span><strong>{esc(str(duration))}</strong></div>
  <div><span>wrk warmup</span><strong>{esc(str(warmup))}</strong></div>
</div>
"""

  scenario_options = ["<option value=\"all\" selected>All scenarios</option>"]
  scenario_options.extend(f"<option value=\"{esc(s)}\">{esc(s)}</option>" for s in scenarios)
  scenario_options_html = "\n".join(scenario_options)

  metric_labels = [
      ("all", "All metrics"),
      ("rps", "Requests/sec (RPS)"),
      ("latency", "Avg Latency (ms)"),
      ("transfer", "Data transferred (MB)"),
  ]
  metric_options_html = "\n".join(
      f"<option value=\"{key}\"{' selected' if key == 'all' else ''}>{label}</option>" for key, label in metric_labels)

  units = {"latency": "ms", "transfer": "MB"}

  def parse_metric_value(value: Any, metric: str) -> Optional[float]:
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
        if unit_part.startswith("us") or unit_part.startswith("µs") or unit_part.startswith("μs"):
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
    }
    for srv in servers:
      numeric["rps"].append(parse_metric_value(scen.get("rps", {}).get(srv), "rps"))
      numeric["latency"].append(parse_metric_value(scen.get("latency", {}).get(srv), "latency"))
      numeric["transfer"].append(parse_metric_value(scen.get("transfer", {}).get(srv), "transfer"))
    chart_payload["scenarios"].append(numeric)

  # Inline the JSON payload directly as a JS object literal. Escape closing
  # </script> sequences to avoid breaking out of the script tag.
  payload_json = json.dumps(chart_payload).replace("</", "<\\/")

  # Prefer loading an external template file if available to keep the
  # Python file small. Fall back to the embedded `template` string above.
  tpl_path = Path(__file__).parent / 'template' / 'benchmarks_template.html'
  if tpl_path.exists():
    tpl = tpl_path.read_text(encoding='utf-8')
  else:
    raise FileNotFoundError(f"Template file not found: {tpl_path}")

  # Fill placeholders safely. `payload_json` already escaped </script> sequences.
  html_out = (tpl
              .replace('__TABLE_HTML__', table_html)
              .replace('__PAYLOAD_JSON__', payload_json)
              .replace('__META_CARDS__', meta_cards)
              .replace('__SCENARIO_OPTIONS__', scenario_options_html)
              .replace('__METRIC_OPTIONS__', metric_options_html))
  return html_out


def main() -> None:
  import argparse

  parser = argparse.ArgumentParser(description="Render HTML from benchmark_latest.json")
  parser.add_argument("--input", type=Path, default=Path("benchmark_latest.json"))
  parser.add_argument("--output", type=Path, default=Path("benchmarks.html"))
  args = parser.parse_args()

  summary = load_summary(args.input)
  html = render_html(summary)
  args.output.write_text(html, encoding="utf-8")


if __name__ == "__main__":
  main()
