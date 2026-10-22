#!/usr/bin/env python3
"""Analyze and report the failures of .github/workflows/flags-combinations.yml.

Two subcommands:

  * "leg": runs at the end of every matrix leg. Extracts the errors of the failed step (configure,
    build or test) from its log into a small JSON result, uploaded as an artifact:
    compiler diagnostics, linker errors, CMake errors, failing GoogleTest cases, crashed or timed
    out tests. Each error gets a fingerprint that ignores line numbers, so that it stays the same
    when unrelated code moves.
  * "report": runs once after all the legs. Groups the failing legs into categories (legs sharing
    at least one error fingerprint), writes a markdown summary with the commands reproducing the
    smallest failing combination of each category and the flag pattern that best explains it,
    and optionally files GitHub issues: one per new category, a comment on the open issue of a
    known one. Issues carry their fingerprints in a hidden marker of their body.

A job log downloaded from GitHub can be analyzed locally as well (timestamps are ignored), e.g.:
  report-flag-matrix.py leg --id 0 --kind wave --cmake-args "..." --outcome build=failure \
      --logs-dir <dir with build.log> --source-dir /home/runner/work/aeronet/aeronet --out leg.json

Only the standard library is required; "report" calls `gh` for the job links and the issues.
"""
import argparse
import hashlib
import itertools
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

LABEL = "flag-combinations"
STEPS = ("configure", "build", "test")
BUILD_DIR = "build"          # build directory of the legs, relative to the source directory
REPRO_BUILD_DIR = "build-flags"
MAX_ERRORS_PER_LEG = 30
MAX_TARGETS = 10
MAX_EXCERPT_LINES = 30
MAX_LINE_CHARS = 400
MAX_DETAILED_ERRORS = 8      # errors shown with their excerpt in a report section
MAX_PATTERNS = 3             # more candidate trigger patterns than this are not worth listing
MAX_ISSUE_BODY = 60000       # GitHub rejects bodies longer than 65536 characters

MARKER_RE = re.compile(r"<!-- flag-combinations-fingerprints: ([0-9a-z ]*) -->")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
TIMESTAMP_RE = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?Z ")  # downloaded job logs
FLAG_RE = re.compile(r"-D\s*(\w+)=(ON|OFF)\b")

CMAKE_ERROR_RE = re.compile(
    r"^CMake Error(?: \(dev\))?(?: (?:at|in) (?P<loc>.+?)(?::\d+)?(?: \(\w+\))?)?:\s*(?P<rest>.*)$")
NINJA_FAILED_RE = re.compile(r"^FAILED: (?:\[code=\d+\] )?(?P<outputs>.+?)\s*$")
NINJA_PROGRESS_RE = re.compile(r"^\[\d+/\d+\] ")
COMPILER_DIAG_RE = re.compile(
    r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?:\d+:)? (?:fatal error|error): (?P<msg>.+)$")
INCLUDED_FROM_RE = re.compile(r"^(?:In file included from |\s+from )")
UNDEFINED_REF_RE = re.compile(r"undefined reference to [`'](?P<sym>.+)'\s*$")
MULTIPLE_DEF_RE = re.compile(r"multiple definition of [`'](?P<sym>.+?)'")
LLD_ERROR_RE = re.compile(r"^(?:\S*/)?ld\.lld(?:-\d+)?: error: (?P<msg>.+)$")
DRIVER_ERROR_RE = re.compile(r"^(?:\S*/)?(?:clang\+\+|clang|g\+\+|gcc|c\+\+)(?:-\d+)?: error: (?P<msg>.+)$")
CTEST_STATUS_RE = re.compile(r"^\s*(?:\d+/\d+\s+)?Test\s+#\d+: (?P<name>\S+) \.*\s*(?P<status>.+?)\s+[\d.]+ sec$")
CTEST_FINAL_RE = re.compile(r"^\s+\d+ - (?P<name>\S+) \((?P<reason>[^)]+)\)")
GTEST_RUN_RE = re.compile(r"^\[ RUN      \] (?P<case>\S+)$")
GTEST_END_RE = re.compile(r"^\[(?:       OK |  FAILED  |  SKIPPED )\] (?P<case>[^\s,]+)(?P<where>, where .+)? \(\d+ ms\)$")
GTEST_FAILURE_RE = re.compile(r": Failure$")


def fingerprint(key):
    return hashlib.sha1(key.encode()).hexdigest()[:12]


def squash(text):
    return " ".join(text.split())


def read_lines(path):
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return []
    return [TIMESTAMP_RE.sub("", ANSI_RE.sub("", line)) for line in text.splitlines()]


class Paths:
    """Shortens the absolute paths of the CI runner to paths relative to the source directory."""

    def __init__(self, source_dir):
        self.source = os.path.abspath(source_dir)
        self.build = os.path.join(self.source, BUILD_DIR)

    def rel(self, path, base=None):
        full = os.path.normpath(path if os.path.isabs(path) else os.path.join(base or self.build, path))
        rel = os.path.relpath(full, self.source)
        return path if rel.startswith("..") else rel

    def strip(self, text):
        return text.replace(self.source + "/", "")


class LegErrors:
    """Distinct errors of one leg, in order of first appearance, keyed by fingerprint."""

    def __init__(self, paths):
        self.paths = paths
        self.errors = {}
        self.truncated = False

    def add(self, key, title, excerpt):
        fp = fingerprint(key)
        if fp in self.errors:
            return
        if len(self.errors) == MAX_ERRORS_PER_LEG:
            self.truncated = True
            return
        lines = [self.paths.strip(line)[:MAX_LINE_CHARS] for line in excerpt]
        while lines and not lines[-1].strip():
            lines.pop()
        if len(lines) > MAX_EXCERPT_LINES:
            lines = lines[:MAX_EXCERPT_LINES] + ["[...]"]
        self.errors[fp] = {"fp": fp, "title": self.paths.strip(title)[:300], "excerpt": "\n".join(lines)}

    def __len__(self):
        return len(self.errors)


def parse_cmake_errors(lines, errors):
    for i, line in enumerate(lines):
        match = CMAKE_ERROR_RE.match(line)
        if not match:
            continue
        end = i + 1
        while end < len(lines) and (not lines[end] or lines[end][0].isspace()
                                    or lines[end].startswith("Call Stack")):
            end += 1
        message = match["rest"] or next((l.strip() for l in lines[i + 1:end] if l.strip()), "")
        loc = errors.paths.rel(match["loc"], errors.paths.source) if match["loc"] else ""
        where = f" at {loc}" if loc else ""
        errors.add(f"cmake|{loc}|{squash(message)}", f"CMake error{where}: {squash(message)}", lines[i:end])


def ninja_edges(lines):
    """Failed ninja edges: (first output, output lines without the command line)."""
    edges, current = [], None
    for line in lines:
        match = NINJA_FAILED_RE.match(line)
        if match:
            current = (match["outputs"].split()[0], [])
            edges.append(current)
        elif current is not None:
            if NINJA_PROGRESS_RE.match(line) or line.startswith("ninja: "):
                current = None
            else:
                current[1].append(line)
    return [(target, body[1:]) for target, body in edges]


def diagnostic_excerpt(body, i):
    start = i
    while start > 0 and INCLUDED_FROM_RE.match(body[start - 1]):
        start -= 1
    end = i + 1
    while end < len(body) and not COMPILER_DIAG_RE.match(body[end]) and not INCLUDED_FROM_RE.match(body[end]) \
            and not re.match(r"^\d+ errors? generated\.$", body[end]):
        end += 1
    return body[start:end]


def parse_edge(body, errors):
    """Adds the errors of one failed ninja edge; returns whether at least one was recognised."""
    count = len(errors)
    paths = errors.paths
    for i, line in enumerate(body):
        if match := UNDEFINED_REF_RE.search(line):
            sym = match["sym"]
            errors.add(f"link|undefined|{sym}", f"undefined reference to `{sym}`", body[max(0, i - 1):i + 1])
        elif match := MULTIPLE_DEF_RE.search(line):
            sym = match["sym"]
            errors.add(f"link|multiple|{sym}", f"multiple definition of `{sym}`", body[i:i + 2])
        elif match := LLD_ERROR_RE.match(line):
            end = i + 1
            while end < len(body) and body[end].startswith(">>>"):
                end += 1
            errors.add(f"link|{squash(match['msg'])}", f"link error: {squash(match['msg'])}", body[i:end])
        elif match := COMPILER_DIAG_RE.match(line):
            file, msg = paths.rel(match["file"]), squash(match["msg"])
            errors.add(f"compile|{file}|{msg}", f"{file}: {msg}", diagnostic_excerpt(body, i))
    if len(errors) == count:
        parse_cmake_errors(body, errors)   # build.ninja regeneration
    if len(errors) == count:
        for line in body:
            if match := DRIVER_ERROR_RE.match(line):
                msg = squash(match["msg"])
                errors.add(f"driver|{msg}", msg, body[-MAX_EXCERPT_LINES:])
    return len(errors) > count


def parse_build(lines, errors, leg):
    edges = ninja_edges(lines)
    for target, body in edges:
        if target not in leg["targets"] and len(leg["targets"]) < MAX_TARGETS:
            leg["targets"].append(target)
        if not parse_edge(body, errors):
            errors.add(f"build|{target}", f"build of {target} failed", body[-MAX_EXCERPT_LINES:])
    if not edges:
        parse_cmake_errors(lines, errors)


def gtest_failures(block):
    """(case, excerpt) of the failed GoogleTest cases of a test output, plus the case left running."""
    start, failures, running = {}, [], None
    for i, line in enumerate(block):
        if match := GTEST_RUN_RE.match(line):
            start[match["case"]] = i
            running = match["case"]
        elif match := GTEST_END_RE.match(line):
            case = match["case"]
            running = None if running == case else running
            if line.startswith("[  FAILED  ]") and case in start:
                span = block[start[case] + 1:i + 1]
                first = next((j for j, l in enumerate(span) if GTEST_FAILURE_RE.search(l)), None)
                failures.append((case, span[first:] if first is not None else span[-MAX_EXCERPT_LINES:]))
    return failures, running


def parse_test(lines, errors, leg):
    final, blocks, current, in_final = {}, {}, None, False
    for line in lines:
        if line.startswith("The following tests FAILED:"):
            in_final, current = True, None
        elif in_final and (match := CTEST_FINAL_RE.match(line)):
            final[match["name"]] = match["reason"]
        elif match := CTEST_STATUS_RE.match(line):
            in_final = False
            current = None
            if match["status"] not in ("Passed", "Not Run"):
                current = []
                blocks.setdefault(match["name"], []).append(current)
        elif re.match(r"^\d+% tests passed", line):
            current = None
        elif current is not None:
            current.append(line)

    for name, reason in final.items():
        leg["tests"].append(name)
        recognised, running, last = False, None, []
        for block in blocks.get(name, []):
            failures, block_running = gtest_failures(block)
            for case, excerpt in failures:
                errors.add(f"test|{name}|{case}", f"{name}: {case} failed", excerpt)
                if case not in leg["cases"]:
                    leg["cases"].append(case)
                recognised = True
            running, last = block_running or running, block or last
        if not recognised:
            what = f"{running} ({reason})" if running else reason
            errors.add(f"test|{name}|{running or reason}", f"{name}: {what}", last[-MAX_EXCERPT_LINES:])
            if running and running not in leg["cases"]:
                leg["cases"].append(running)


def test_executables(paths, names):
    """Executables (relative to the build directory, thus ninja targets) of the given ctest tests."""
    try:
        out = subprocess.run(["ctest", "--test-dir", paths.build, "--show-only=json-v1"],
                             capture_output=True, text=True, check=True, timeout=120).stdout
        tests = json.loads(out).get("tests", [])
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError) as e:
        print(f"warning: cannot list the ctest tests of {paths.build}: {e}", file=sys.stderr)
        return []
    targets = []
    for test in tests:
        command = test.get("command") or []
        if test.get("name") in names and command:
            rel = os.path.relpath(command[0], paths.build)
            if not rel.startswith(".."):
                targets.append(rel)
    return targets


def cmd_leg(args):
    outcomes = dict(o.split("=", 1) for o in args.outcome)
    leg = {"id": args.id, "kind": args.kind, "cmake_args": args.cmake_args, "status": "pass", "step": "",
           "errors": [], "targets": [], "tests": [], "test_targets": [], "cases": [], "truncated": False}
    failed = next((s for s in STEPS if outcomes.get(s) == "failure"), None)
    if failed is None:
        if not all(outcomes.get(s) == "success" for s in STEPS):
            leg["status"] = "setup"   # a step before Configure failed: nothing about the flags
    else:
        leg["status"], leg["step"] = "fail", failed
        paths = Paths(args.source_dir)
        errors = LegErrors(paths)
        lines = read_lines(Path(args.logs_dir) / f"{failed}.log")
        if failed == "configure":
            parse_cmake_errors(lines, errors)
        elif failed == "build":
            parse_build(lines, errors, leg)
        else:
            parse_test(lines, errors, leg)
            leg["test_targets"] = test_executables(paths, set(leg["tests"])) if leg["tests"] else []
        if not errors:
            errors.add(f"{failed}|unrecognised", f"{failed} failed (no recognised error message)",
                       lines[-MAX_EXCERPT_LINES:])
        leg["errors"] = list(errors.errors.values())
        leg["truncated"] = errors.truncated
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(leg, indent=1), encoding="utf-8")
    print(f"leg #{args.id}: {leg['status']}" + (f" at {leg['step']}, {len(leg['errors'])} distinct error(s)"
                                               if leg["step"] else ""))
    for error in leg["errors"]:
        print(f"  [{error['fp']}] {error['title']}")


# ----------------------------------------------------------------------------------------------
# report

def flags_of(cmake_args):
    return dict(FLAG_RE.findall(cmake_args))


def short_flag(name):
    return name.removeprefix("AERONET_").removeprefix("ENABLE_")


def enabled_flags(cmake_args):
    return ", ".join(short_flag(k) for k, v in flags_of(cmake_args).items() if v == "ON") or "(none)"


def pattern_text(pattern):
    return " + ".join(f"`{short_flag(k)}={v}`" for k, v in pattern)


class Category:
    """Failing legs sharing errors, transitively. Other kinds of categories have a single fixed error."""

    def __init__(self, legs, kind="fail", title=""):
        self.legs = sorted(legs, key=lambda leg: leg["id"])
        self.kind = kind
        counts, errors = {}, {}
        if kind != "fail":
            errors = {fingerprint(kind): {"fp": fingerprint(kind), "title": title, "excerpt": ""}}
            counts = {fingerprint(kind): len(legs)}
        for leg in self.legs:
            for error in leg["errors"]:
                errors.setdefault(error["fp"], error)
                counts[error["fp"]] = counts.get(error["fp"], 0) + 1
        order = list(errors)
        self.errors = sorted(errors.values(), key=lambda e: (-counts[e["fp"]], order.index(e["fp"])))
        self.counts = counts
        self.fps = set(errors)
        # The failing combination with the fewest enabled options is the cheapest to reproduce.
        self.repro = min(self.legs, key=lambda leg: (list(flags_of(leg["cmake_args"]).values()).count("ON"),
                                                     leg["id"]), default=None)
        self.issues = []   # (number, url, "new" | "updated")
        self.previous = []  # closed issues with the same errors

    @property
    def title(self):
        more = f" (+{len(self.errors) - 1} more errors)" if len(self.errors) > 1 else ""
        return self.errors[0]["title"] + more


def categorize(matrix, results):
    failing = [r for r in results.values() if r["status"] == "fail"]
    parent = {leg["id"]: leg["id"] for leg in failing}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    owner = {}
    for leg in failing:
        for error in leg["errors"]:
            other = owner.setdefault(error["fp"], leg["id"])
            parent[find(leg["id"])] = find(other)
    groups = {}
    for leg in failing:
        groups.setdefault(find(leg["id"]), []).append(leg)
    categories = sorted((Category(legs) for legs in groups.values()), key=lambda c: (-len(c.legs), c.legs[0]["id"]))

    missing = [dict(leg, status="missing", step="", errors=[], targets=[], tests=[], test_targets=[], cases=[])
               for leg in matrix if leg["id"] not in results]
    if missing:
        categories.append(Category(missing, "no-result", "leg produced no result (timed out, cancelled, runner lost or error extraction failed)"))
    setup = [r for r in results.values() if r["status"] == "setup"]
    if setup:
        categories.append(Category(setup, "setup", "CI setup failed before configuring (dependencies, toolchain)"))
    if not matrix:
        categories.append(Category([], "no-matrix", "workflow failed before running the legs"))
    return categories


def trigger_patterns(category, passing):
    """Smallest flag patterns shared by all the legs of the category and by no passing leg."""
    flags = [flags_of(leg["cmake_args"]) for leg in category.legs]
    if not flags:
        return {}, []
    shared = {k: v for k, v in flags[0].items() if all(f.get(k) == v for f in flags[1:])}
    passing_flags = [flags_of(leg["cmake_args"]) for leg in passing]
    for size in range(1, 4):
        patterns = [combo for combo in itertools.combinations(sorted(shared.items()), size)
                    if not any(all(p.get(k) == v for k, v in combo) for p in passing_flags)]
        if patterns:
            return shared, patterns
    return shared, []


def fence(text, lang="text"):
    longest = max((len(m) for m in re.findall(r"`+", text)), default=0)
    ticks = "`" * max(3, longest + 1)
    return f"{ticks}{lang}\n{text}\n{ticks}"


class Context:
    def __init__(self, args, matrix, results, job_urls):
        self.args = args
        self.matrix = matrix
        self.results = results
        self.job_urls = job_urls
        self.passing = [r for r in results.values() if r["status"] == "pass"]
        self.run_url = f"{args.server_url}/{args.repo}/actions/runs/{args.run_id}" if args.run_id else ""
        self.fixed_args = shlex.split(args.fixed_args)

    def run_ref(self):
        run = f"[#{self.args.run_number}]({self.run_url})" if self.run_url else f"#{self.args.run_number or '?'}"
        sha = self.args.sha[:10]
        commit = f"[`{sha}`]({self.args.server_url}/{self.args.repo}/commit/{self.args.sha})" if sha else "unknown commit"
        return f"run {run} at commit {commit}"

    def leg_ref(self, leg):
        url = self.job_urls.get(leg["id"])
        return f"[#{leg['id']}]({url})" if url else f"#{leg['id']}"


def repro_commands(leg, ctx):
    args = ctx.fixed_args + shlex.split(leg["cmake_args"])
    lines = [f"git checkout {ctx.args.sha or '<commit>'}",
             f"cmake -S . -B {REPRO_BUILD_DIR} -G Ninja \\"]
    lines += [f"  {shlex.quote(a)}" + (" \\" if i + 1 < len(args) else "") for i, a in enumerate(args)]
    if leg["step"] == "build":
        lines.append(" ".join(["ninja", "-C", REPRO_BUILD_DIR, *map(shlex.quote, leg["targets"][:5])]))
    elif leg["step"] == "test":
        if leg["test_targets"]:
            lines.append(" ".join(["ninja", "-C", REPRO_BUILD_DIR, *map(shlex.quote, leg["test_targets"])]))
        else:
            lines.append(f"cmake --build {REPRO_BUILD_DIR}")
        regex = "^(" + "|".join(re.sub(r"([.^$*+?()\[\]{}|\\])", r"\\\1", t) for t in leg["tests"]) + ")$"
        env = f"GTEST_FILTER={shlex.quote(':'.join(leg['cases']))} " if leg["cases"] else ""
        lines.append(f"{env}ctest --test-dir {REPRO_BUILD_DIR} -R {shlex.quote(regex)} --output-on-failure --timeout 60")
    elif leg["step"] == "":
        lines.append(f"cmake --build {REPRO_BUILD_DIR} && ctest --test-dir {REPRO_BUILD_DIR} --output-on-failure --timeout 60")
    return "\n".join(lines)


def render_legs_table(category, ctx):
    rows = ["| Leg | Failed step | Enabled options |", "| --- | --- | --- |"]
    for leg in category.legs:
        rows.append(f"| {ctx.leg_ref(leg)} ({leg['kind']}) | {leg['step'] or leg['status']} | "
                    f"{enabled_flags(leg['cmake_args'])} |")
    return "\n".join(rows)


def render_trigger(category, ctx):
    if category.kind != "fail":
        return ""
    shared, patterns = trigger_patterns(category, ctx.passing)
    lines = []
    if ctx.passing:
        if 0 < len(patterns) <= MAX_PATTERNS:
            lines.append(f"Smallest flag patterns shared by every failing leg and by none of the "
                         f"{len(ctx.passing)} passing legs of the run: {' or '.join(map(pattern_text, patterns))}.")
        elif patterns:
            lines.append(f"{len(patterns)} different patterns of {len(patterns[0])} flags separate the failing legs "
                         f"from the {len(ctx.passing)} passing legs of the run: too few failing legs to narrow it down.")
        else:
            lines.append(f"No pattern of up to 3 flags separates the failing legs from the "
                         f"{len(ctx.passing)} passing legs of the run.")
    if len(category.legs) > 1:
        lines.append(f"Values shared by all {len(category.legs)} failing legs: "
                     + (", ".join(f"`{short_flag(k)}={v}`" for k, v in sorted(shared.items())) or "none") + ".")
    if not lines:
        return ""
    return "### Candidate trigger\n\n" + "\n\n".join(lines) + \
        "\n\nThis is a hint computed from this run only: confirm it by toggling these options."


def render_errors(category, new_fps=None):
    parts = []
    errors = [e for e in category.errors if new_fps is None or e["fp"] in new_fps]
    for error in errors[:MAX_DETAILED_ERRORS]:
        count = category.counts.get(error["fp"], 0)
        parts.append(f"**`{error['title']}`** ({count}/{len(category.legs)} legs, fingerprint `{error['fp']}`)")
        if error["excerpt"]:
            parts.append(fence(error["excerpt"]))
    if len(errors) > MAX_DETAILED_ERRORS:
        parts.append("Other errors:\n\n" + "\n".join(f"- `{e['title']}` (`{e['fp']}`)"
                                                     for e in errors[MAX_DETAILED_ERRORS:]))
    if any(leg.get("truncated") for leg in category.legs):
        parts.append(f"Some legs had more than {MAX_ERRORS_PER_LEG} distinct errors: only the first ones are listed.")
    return "\n\n".join(parts)


def render_repro(category, ctx):
    leg = category.repro
    if leg is None:
        return f"See the logs of {ctx.run_ref()}."
    what = {"build": "builds only the failing outputs", "test": "builds and runs only the failing tests",
            "configure": "configures only"}.get(leg["step"], "builds and tests everything")
    return (f"Smallest failing combination: leg {ctx.leg_ref(leg)} ({what}; the CI uses clang-21 "
            f"in Debug mode):\n\n" + fence(repro_commands(leg, ctx), "bash"))


def render_category(category, ctx):
    total = len(ctx.matrix)
    intro = {
        "fail": f"**{len(category.legs)}** of the {total} legs of {ctx.run_ref()} failed with these errors.",
        "no-result": f"**{len(category.legs)}** of the {total} legs of {ctx.run_ref()} produced no result: "
                     f"they timed out (60 minutes), were cancelled, lost their runner, or their error extraction failed. "
                     f"Check their job logs.",
        "setup": f"**{len(category.legs)}** of the {total} legs of {ctx.run_ref()} failed before configuring "
                 f"(dependency or toolchain installation): not related to the flags.",
        "no-matrix": f"The workflow failed before running any leg in {ctx.run_ref()}: check the Generate matrix job.",
    }[category.kind]
    parts = [intro]
    if previous := category.previous:
        refs = ", ".join(f"#{n}" for n in previous)
        parts.append(f"The same errors were reported before in {refs} (closed): a regression, or a fix that "
                     f"does not cover these combinations.")
    if category.kind in ("fail", "no-result"):
        parts += ["### Reproduce", render_repro(category, ctx)]
    if category.kind == "fail":
        parts += ["### Errors", render_errors(category)]
    if trigger := render_trigger(category, ctx):
        parts.append(trigger)
    if category.legs:
        parts += ["### Failing legs", render_legs_table(category, ctx)]
    return "\n\n".join(parts)


def marker(fps):
    return f"<!-- flag-combinations-fingerprints: {' '.join(sorted(fps))} -->"


def issue_body(category, ctx):
    body = "\n\n".join([
        marker(category.fps),
        render_category(category, ctx),
        f"---\nFiled by the periodic [flag-combinations workflow]({ctx.args.server_url}/{ctx.args.repo}/blob/main/"
        ".github/workflows/flags-combinations.yml). "
        "Later runs failing with one of these errors comment here instead of opening a new issue; "
        "close the issue once fixed (a new failure then opens a new issue that links to this one).",
    ])
    return clip_body(body)


def comment_body(category, ctx, new_fps):
    parts = [f"Still failing in {ctx.run_ref()}: {len(category.legs)} of the {len(ctx.matrix)} legs."]
    if new_fps:
        parts += ["New errors in this category:", render_errors(category, new_fps)]
    if trigger := render_trigger(category, ctx):
        parts.append(trigger)
    parts.append("<details><summary>Reproduction and failing legs</summary>\n\n" + render_repro(category, ctx)
                 + "\n\n" + render_legs_table(category, ctx) + "\n\n</details>")
    return clip_body("\n\n".join(parts))


def clip_body(body):
    if len(body) <= MAX_ISSUE_BODY:
        return body
    return body[:MAX_ISSUE_BODY] + "\n\n[... truncated: see the summary of the workflow run]"


class GhError(Exception):
    pass


def gh(*args, stdin=None):
    try:
        result = subprocess.run(["gh", *args], input=stdin, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as e:
        raise GhError(f"gh {' '.join(args[:2])}: {e}") from e
    if result.returncode != 0:
        raise GhError(f"gh {' '.join(args[:2])} failed ({result.returncode}): {result.stderr.strip()}")
    return result.stdout


def fetch_job_urls(args):
    if not (args.repo and args.run_id):
        return {}
    try:
        out = gh("api", "--paginate", f"repos/{args.repo}/actions/runs/{args.run_id}/attempts/{args.run_attempt}/jobs",
                 "--jq", ".jobs[] | [.name, .html_url] | @tsv")
    except GhError as e:
        print(f"warning: cannot list the jobs of run {args.run_id}, legs will not link to their logs: {e}",
              file=sys.stderr)
        return {}
    urls = {}
    for line in out.splitlines():
        name, _, url = line.partition("\t")
        if match := re.search(r"#(\d+)$", name):
            urls[int(match[1])] = url
    return urls


def file_issues(categories, ctx):
    """Opens or updates one issue per category; returns the number of gh failures."""
    args, repo = ctx.args, ctx.args.repo
    dry = args.dry_run
    issues = json.loads(gh("issue", "list", "--repo", repo, "--label", LABEL, "--state", "all", "--limit", "500",
                           "--json", "number,state,body,url"))
    for issue in issues:
        match = MARKER_RE.search(issue["body"] or "")
        issue["fps"] = set(match[1].split()) if match else set()
    issues.sort(key=lambda i: i["number"])
    if not dry:
        gh("label", "create", LABEL, "--repo", repo, "--force", "--color", "d93f0b",
           "--description", "Failure found by the periodic flag-combinations workflow")

    failures = 0
    for category in categories:
        if category.kind == "setup":
            continue
        matching = [i for i in issues if i["state"] == "OPEN" and i["fps"] & category.fps]
        category.previous = [i["number"] for i in issues if i["state"] != "OPEN" and i["fps"] & category.fps]
        try:
            if not matching:
                body = issue_body(category, ctx)
                title = f"Flag combinations: {category.title}"[:200]
                if dry:
                    print(f"\n[dry-run] gh issue create --title {title!r}\n{body}\n")
                    continue
                url = gh("issue", "create", "--repo", repo, "--title", title, "--label", LABEL,
                         "--body-file", "-", stdin=body).strip()
                number = int(url.rstrip("/").rsplit("/", 1)[-1])
                category.issues.append((number, url, "new"))
                issues.append({"number": number, "state": "OPEN", "body": body, "url": url, "fps": set(category.fps)})
                continue
            known = set().union(*(i["fps"] for i in matching))
            new_fps = category.fps - known
            body = comment_body(category, ctx, new_fps)
            for issue in matching:
                if dry:
                    print(f"\n[dry-run] gh issue comment {issue['number']}\n{body}\n")
                else:
                    gh("issue", "comment", str(issue["number"]), "--repo", repo, "--body-file", "-", stdin=body)
                category.issues.append((issue["number"], issue["url"], "updated"))
            if new_fps:
                first = matching[0]
                first["fps"] |= new_fps
                updated = MARKER_RE.sub(marker(first["fps"]), first["body"], count=1)
                if dry:
                    print(f"[dry-run] gh issue edit {first['number']}: fingerprints += {sorted(new_fps)}")
                else:
                    gh("issue", "edit", str(first["number"]), "--repo", repo, "--body-file", "-", stdin=updated)
                first["body"] = updated
        except GhError as e:
            failures += 1
            print(f"error: cannot report category '{category.title}' ({len(category.legs)} legs): {e}",
                  file=sys.stderr)
    return failures


def render_summary(categories, ctx):
    failing = sum(len(c.legs) for c in categories if c.kind == "fail")
    count = sum(c.kind == "fail" for c in categories)
    lines = ["## Flag combination failures\n",
             f"{failing} of the {len(ctx.matrix)} legs failed, in {count} distinct "
             f"{'category' if count == 1 else 'categories'} (legs sharing an error are grouped together)."]
    for n, category in enumerate(categories, 1):
        issues = ", ".join(f"[#{num}]({url}) ({state})" for num, url, state in category.issues)
        lines.append(f"\n### {n}. `{category.title}`" + (f" - {issues}" if issues else ""))
        lines.append(render_category(category, ctx).replace("\n### ", "\n#### "))
    return "\n".join(lines) + "\n"


def cmd_report(args):
    try:
        matrix = json.loads(args.matrix).get("include", []) if args.matrix.strip() else []
    except json.JSONDecodeError as e:
        print(f"error: invalid matrix ({e}): {args.matrix[:200]!r}", file=sys.stderr)
        matrix = []
    results = {}
    for path in sorted(Path(args.results_dir).glob("**/*.json")):
        leg = json.loads(path.read_text(encoding="utf-8"))
        results[leg["id"]] = leg
    ctx = Context(args, matrix, results, fetch_job_urls(args))
    categories = categorize(matrix, results)

    failures = 0
    if args.file_issues and categories:
        try:
            failures = file_issues(categories, ctx)
        except GhError as e:
            failures = 1
            print(f"error: cannot list or label the issues of {args.repo}: {e}", file=sys.stderr)

    summary = render_summary(categories, ctx)
    if args.summary:
        with open(args.summary, "a", encoding="utf-8") as f:
            f.write(summary)
    else:
        print(summary)
    for category in categories:
        print(f"{len(category.legs):3} legs  {category.kind:9}  {category.title}")
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="command", required=True)

    leg = sub.add_parser("leg", help="extract the errors of one matrix leg")
    leg.add_argument("--id", type=int, required=True)
    leg.add_argument("--kind", required=True)
    leg.add_argument("--cmake-args", required=True,
                     help="CMake options of the leg (matrix.cmake_args), pass it as --cmake-args=...")
    leg.add_argument("--outcome", action="append", default=[], metavar="STEP=OUTCOME",
                     help="outcome of the configure / build / test step (steps.<id>.outcome)")
    leg.add_argument("--logs-dir", default="logs", help="directory of the <step>.log files")
    leg.add_argument("--source-dir", default=".", help="source directory, stripped from paths")
    leg.add_argument("--out", required=True, help="JSON result file")

    rep = sub.add_parser("report", help="group the leg results and report them")
    rep.add_argument("--matrix", default="", help="matrix JSON of gen-flag-matrix.py")
    rep.add_argument("--results-dir", required=True, help="directory of the leg JSON results")
    rep.add_argument("--fixed-args", default=os.environ.get("CMAKE_FIXED_ARGS", ""),
                     help="CMake options common to all the legs (default: $CMAKE_FIXED_ARGS)")
    rep.add_argument("--summary", help="append the markdown summary to this file (default: stdout)")
    rep.add_argument("--file-issues", action="store_true", help="open / comment GitHub issues")
    rep.add_argument("--dry-run", action="store_true", help="print the issue actions instead of running them")
    rep.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    rep.add_argument("--server-url", default=os.environ.get("GITHUB_SERVER_URL", "https://github.com"))
    rep.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", ""))
    rep.add_argument("--run-number", default=os.environ.get("GITHUB_RUN_NUMBER", ""))
    rep.add_argument("--run-attempt", default=os.environ.get("GITHUB_RUN_ATTEMPT", "1"))
    rep.add_argument("--sha", default=os.environ.get("GITHUB_SHA", ""))

    args = ap.parse_args()
    if args.command == "leg":
        cmd_leg(args)
        return 0
    if args.file_issues and not args.repo:
        ap.error("--file-issues requires --repo (or GITHUB_REPOSITORY)")
    return cmd_report(args)


if __name__ == "__main__":
    sys.exit(main())
