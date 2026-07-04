#!/usr/bin/env python3
"""Render a Markdown file (the repo README) to a self-contained HTML homepage.

The GitHub Pages site is deployed as a *static* GitHub Actions artifact (no Jekyll), so a
root `index.md` is not turned into `index.html` automatically the way the old branch-based
Jekyll build did. This script fills that gap: it converts the README to a standalone,
theme-aware `index.html` used as the site landing page.

Usage: render_readme_html.py <input.md> <output.html> [--title TITLE]
"""

from __future__ import annotations

import argparse
import html
import sys

import markdown

# Minimal, dependency-free, responsive, light/dark-aware styling (GitHub-README-like).
_TEMPLATE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
:root {{ color-scheme: light dark; }}
* {{ box-sizing: border-box; }}
body {{
  margin: 0; padding: 2.5rem 1rem;
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Helvetica, Arial, sans-serif;
  line-height: 1.6; color: #1f2328; background: #ffffff;
}}
main {{ max-width: 900px; margin: 0 auto; }}
a {{ color: #0969da; text-decoration: none; }}
a:hover {{ text-decoration: underline; }}
h1, h2, h3, h4 {{ line-height: 1.25; margin-top: 1.8rem; }}
h1, h2 {{ border-bottom: 1px solid #d0d7de; padding-bottom: .3em; }}
code {{
  font-family: ui-monospace, SFMono-Regular, "SF Mono", Menlo, monospace;
  font-size: .9em; background: rgba(129,139,152,.15);
  padding: .2em .4em; border-radius: 6px;
}}
pre {{ background: #f6f8fa; padding: 1rem; border-radius: 6px; overflow-x: auto; }}
pre code {{ background: transparent; padding: 0; }}
table {{ border-collapse: collapse; display: block; overflow-x: auto; }}
th, td {{ border: 1px solid #d0d7de; padding: .4em .8em; }}
img {{ max-width: 100%; }}
blockquote {{ margin: 0; padding: 0 1em; color: #59636e; border-left: .25em solid #d0d7de; }}
@media (prefers-color-scheme: dark) {{
  body {{ color: #e6edf3; background: #0d1117; }}
  a {{ color: #4493f8; }}
  h1, h2 {{ border-bottom-color: #30363d; }}
  pre {{ background: #161b22; }}
  th, td {{ border-color: #30363d; }}
  blockquote {{ color: #9198a1; border-left-color: #30363d; }}
}}
</style>
</head>
<body>
<main>
{body}
</main>
</body>
</html>
"""


def render(md_text: str, title: str) -> str:
    body = markdown.markdown(
        md_text,
        extensions=["fenced_code", "tables", "sane_lists", "toc", "attr_list"],
        output_format="html5",
    )
    return _TEMPLATE.format(title=html.escape(title), body=body)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="input Markdown file")
    parser.add_argument("output", help="output HTML file")
    parser.add_argument("--title", default="aeronet", help="HTML document title")
    args = parser.parse_args(argv)

    with open(args.input, "r", encoding="utf-8") as fh:
        md_text = fh.read()

    with open(args.output, "w", encoding="utf-8") as fh:
        fh.write(render(md_text, args.title))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
