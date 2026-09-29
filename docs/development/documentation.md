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

## Periodic CMake option combinations

Most CMake options are independent, so the number of combinations is far too large for the regular CI, which only builds a curated set of them on every push and pull request. To catch breakage in the untested combinations on a best-effort basis, the `.github/workflows/flag-combinations.yml` workflow runs weekly (and on demand through `workflow_dispatch`). It builds a random sample of option combinations with clang-21 in Debug mode and runs `ctest` on each, and it opens a GitHub issue when a scheduled run fails. The combinations are produced by [scripts/gen-flag-matrix.py](../../scripts/gen-flag-matrix.py), which is seeded (the run number by default, shown in the run summary), so a failing combination can be reproduced by running `python3 scripts/gen-flag-matrix.py --count <count> --seed <seed>` locally or by triggering the workflow manually with the same inputs. When adding a new boolean CMake option, append its name to `FLAGS` in that script, and if the option has prerequisites that CMake enforces (a `FATAL_ERROR` or a `cmake_dependent_option`), encode them in `is_valid()` so that the generator never produces a configuration that cannot exist.
