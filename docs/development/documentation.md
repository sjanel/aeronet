# Documentation development

The documentation site is generated with Material for MkDocs. Its source is the repository's `docs/` directory and its navigation lives in [mkdocs.yml](../../mkdocs.yml).

Keep links to repository source files relative, so they work when the Markdown is read on GitHub or from a local checkout. During an MkDocs build, [scripts/mkdocs_source_links.py](../../scripts/mkdocs_source_links.py) rewrites existing links outside `docs/` to the matching GitHub source URL.

## Preview locally

Create an isolated Python environment, install the documentation dependency, and start the live-reload server:

```bash
python3 -m venv /tmp/aeronet-docs-venv
/tmp/aeronet-docs-venv/bin/python -m pip install --requirement requirements-docs.txt
/tmp/aeronet-docs-venv/bin/python -m mkdocs serve
```

Open the local address reported by MkDocs. Use Ctrl+C to stop the server.

## Validate a production build

```bash
/tmp/aeronet-docs-venv/bin/python -m mkdocs build --strict --site-dir /tmp/aeronet-docs-site
```

`--strict` turns MkDocs warnings into build failures, including navigation and Markdown-link issues that would otherwise reach the published site.

## Publishing

The existing GitHub Pages workflow builds the site and overlays live benchmark dashboards at `/benchmarks/`. It also retains the source Markdown under `/docs/` to avoid breaking existing direct links. Do not add a second Pages deployment workflow: GitHub Pages has one active deployment target, so independent workflows would overwrite each other's published artifact.

When adding C++ code fences, include the page in the Markdown example verifier in `.github/workflows/ci.yml`. The verifier compiles and links documentation snippets against the configured aeronet build.
