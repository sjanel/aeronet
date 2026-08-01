#!/usr/bin/env python3
"""Validate internal Markdown links and build C/C++ documentation fences."""

import argparse
import concurrent.futures
import json
import os
import platform
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Sequence, Tuple
from urllib.parse import unquote, urlsplit


LANGUAGES = {"c", "cpp", "c++"}
MARKDOWN_LINK_PATTERN = re.compile(r"(?<![!\\])\[([^]\n]+)\]\(([^)\n]+)\)")
MARKDOWN_FENCE_PATTERN = re.compile(r"^\s*(`{3,}|~{3,})")
INLINE_CODE_PATTERN = re.compile(r"(`+).*?\1")

DEPENDENCY_LIBRARY_PATTERNS = [
    # zlib: prefer zlib-ng when available; fallback to classic zlib
    ("libz-ng*.a", "-lz-ng"),
    ("libz-ng*.so*", "-lz-ng"),
    ("libz.a", "-lz"),
    ("libz.so*", "-lz"),
    ("libzlib*.a", "-lz"),
    ("libzlib*.so*", "-lz"),
    ("libzstd*.a", "-lzstd"),
    ("libbrotlienc*.a", "-lbrotlienc"),
    ("libbrotlidec*.a", "-lbrotlidec"),
    ("libbrotlicommon*.a", "-lbrotlicommon"),
    # OpenTelemetry libraries - only include those that are actually built.
    # Note: patterns with wildcards (like *_http*.a) can match multiple libs,
    # so we use exact names where possible to avoid duplicates.
    # Link base OTLP exporter library first (contains class implementations)
    ("libopentelemetry_exporter_otlp.a", "-lopentelemetry_exporter_otlp"),
    ("libopentelemetry_exporter_otlp_http.a", "-lopentelemetry_exporter_otlp_http"),
    (
        "libopentelemetry_exporter_otlp_http_metric.a",
        "-lopentelemetry_exporter_otlp_http_metric",
    ),
    (
        "libopentelemetry_exporter_otlp_http_client.a",
        "-lopentelemetry_exporter_otlp_http_client",
    ),
    (
        "libopentelemetry_exporter_ostream_metrics.a",
        "-lopentelemetry_exporter_ostream_metrics",
    ),
    ("libopentelemetry_http_client_curl.a", "-lopentelemetry_http_client_curl"),
    ("libopentelemetry_otlp_recordable.a", "-lopentelemetry_otlp_recordable"),
    ("libopentelemetry_proto.a", "-lopentelemetry_proto"),
    ("libopentelemetry_trace.a", "-lopentelemetry_trace"),
    ("libopentelemetry_metrics.a", "-lopentelemetry_metrics"),
    ("libopentelemetry_logs.a", "-lopentelemetry_logs"),
    ("libopentelemetry_common.a", "-lopentelemetry_common"),
    ("libopentelemetry_resources.a", "-lopentelemetry_resources"),
    ("libopentelemetry_version.a", "-lopentelemetry_version"),
    # Also look for shared object variants (.so) - LLD may require them
    ("libopentelemetry_exporter_otlp.so*", "-lopentelemetry_exporter_otlp"),
    ("libopentelemetry_exporter_otlp_http.so*", "-lopentelemetry_exporter_otlp_http"),
    ("libopentelemetry_exporter_otlp_http_metric.so*", "-lopentelemetry_exporter_otlp_http_metric"),
    ("libopentelemetry_exporter_otlp_http_client.so*", "-lopentelemetry_exporter_otlp_http_client"),
    ("libopentelemetry_trace.so*", "-lopentelemetry_trace"),
    ("libopentelemetry_metrics.so*", "-lopentelemetry_metrics"),
    ("libopentelemetry_common.so*", "-lopentelemetry_common"),
]

SYSTEM_LINK_FLAGS = [
    "-lssl",
    "-lcrypto",
    "-lcurl",
    "-lprotobuf",
]


@dataclass
class Snippet:
    index: int
    source: Path
    source_line: int
    lines: List[str]
    needs_std_module: bool = False
    needs_aeronet_module: bool = False


@dataclass
class SnippetBatch:
    snippets: List[Snippet]
    path: Path
    target: str
    rel_source: Path
    needs_std_module: bool = False
    needs_aeronet_module: bool = False


@dataclass
class TargetUsage:
    include_dirs: List[str]
    compile_definitions: List[str]
    compile_options: List[str]


@dataclass
class InternalLinkFailure:
    source: Path
    source_line: int
    destination: str
    resolved_path: Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate internal Markdown links and compile/link C/C++ fences to detect "
            "documentation bitrot. "
            "Snippets are built through CMake/Ninja against the existing aeronet "
            "build so they automatically inherit include paths, defines, and "
            "toolchain settings."
        )
    )
    parser.add_argument(
        "--include",
        action="append",
        metavar="DIR",
        help="Extra include root to append to the snippet targets",
        default=[],
    )
    parser.add_argument(
        "--link",
        action="store_true",
        help="Link each snippet batch into an executable (default: compile only)",
    )
    parser.add_argument(
        "--build-dir",
        default="build",
        help="Existing aeronet build directory (default: build)",
    )
    parser.add_argument(
        "--cmake",
        default=os.environ.get("CMAKE", "cmake"),
        help="cmake executable to invoke (default: cmake)",
    )
    parser.add_argument(
        "--generator",
        default=os.environ.get("CMAKE_GENERATOR", "Ninja"),
        help="CMake generator to use for the snippet project (default: Ninja)",
    )
    parser.add_argument(
        "--cmake-build-type",
        default=None,
        help="Force CMAKE_BUILD_TYPE for the snippet project (default: reuse aeronet build)",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=None,
        help="Number of batched snippet targets to build in parallel",
    )
    parser.add_argument(
        "--snippets-per-batch",
        "--batch-size",
        type=positive_int,
        default=8,
        metavar="COUNT",
        help=(
            "Maximum independently-scoped snippets per translation unit "
            "(default: 8)"
        ),
    )
    parser.add_argument(
        "files",
        nargs="*",
        default=["README.md", "FEATURES.md"],
        help="Markdown files to scan for code snippets and internal links",
    )
    return parser.parse_args()


def positive_int(value: str) -> int:
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return number


def find_internal_link_failures(
    sources: Sequence[Path],
) -> Tuple[int, List[InternalLinkFailure]]:
    checked_links = 0
    failures: List[InternalLinkFailure] = []

    for source in sources:
        fence = ""
        for line_number, line in enumerate(source.read_text().splitlines(), start=1):
            fence_match = MARKDOWN_FENCE_PATTERN.match(line)
            if fence_match:
                delimiter = fence_match.group(1)
                if fence:
                    if delimiter[0] == fence[0] and len(delimiter) >= len(fence):
                        fence = ""
                else:
                    fence = delimiter
                continue
            if fence:
                continue

            code_spans: List[str] = []

            def mask_code_span(match: re.Match[str]) -> str:
                marker = f"\x00{len(code_spans)}\x00"
                code_spans.append(match.group(0))
                return marker

            markdown_line = INLINE_CODE_PATTERN.sub(mask_code_span, line)
            for match in MARKDOWN_LINK_PATTERN.finditer(markdown_line):
                destination = match.group(2).strip()
                if destination.startswith("<") and destination.endswith(">"):
                    destination = destination[1:-1]
                parsed = urlsplit(destination)
                if parsed.scheme or parsed.netloc or not parsed.path:
                    continue

                checked_links += 1
                resolved_path = source.parent / unquote(parsed.path)
                if resolved_path.exists():
                    continue
                failures.append(
                    InternalLinkFailure(
                        source=source,
                        source_line=line_number,
                        destination=destination,
                        resolved_path=resolved_path,
                    )
                )

    return checked_links, failures


def print_internal_link_failures(failures: Sequence[InternalLinkFailure]) -> None:
    for failure in failures:
        print(
            f"{failure.source}:{failure.source_line}: internal Markdown link "
            f"target does not exist: {failure.destination} "
            f"(resolved to {failure.resolved_path})",
            file=sys.stderr,
        )


def dedupe_preserve(items: Sequence[str]) -> List[str]:
    seen = set()
    ordered: List[str] = []
    for item in items:
        if item in seen:
            continue
        seen.add(item)
        ordered.append(item)
    return ordered


def gather_user_include_dirs(user_includes: Sequence[str]) -> List[str]:
    dirs: List[str] = []
    for include in user_includes:
        path = Path(include)
        dirs.append(str(path.resolve()))
    return dirs


def fallback_include_dirs() -> List[str]:
    possible_roots = [
        "aeronet",
        "aeronet/client/include",
        "aeronet/http/include",
        "aeronet/http2/include",
        "aeronet/jwt/include",
        "aeronet/main/include",
        "aeronet/objects/include",
        "aeronet/server/include",
        "aeronet/sys/include",
        "aeronet/tech/include",
        "aeronet/tls/include",
        "aeronet/websocket/include",
    ]
    dirs: List[str] = []
    for root in possible_roots:
        path = Path(root)
        if path.is_dir():
            dirs.append(str(path.resolve()))
    return dirs


def locate_dependency_include_dirs(build_dir: Path) -> List[str]:
    include_dirs: List[str] = []
    candidates = [
        build_dir / "_deps" / "brotli-src" / "c" / "include",
        build_dir / "_deps" / "spdlog-src" / "include",
        build_dir / "_deps" / "amadeusamc-src" / "include",
        build_dir / "_deps" / "opentelemetry_cpp-src" / "api" / "include",
        build_dir / "_deps" / "opentelemetry_cpp-src" / "sdk" / "include",
        build_dir
        / "_deps"
        / "opentelemetry_cpp-src"
        / "exporters"
        / "ostream"
        / "include",
        build_dir
        / "_deps"
        / "opentelemetry_cpp-src"
        / "exporters"
        / "otlp"
        / "include",
        build_dir / "_deps" / "opentelemetry_cpp-src" / "ext" / "include",
        build_dir / "generated" / "third_party" / "opentelemetry-proto",
        build_dir / "_deps" / "zlib-ng-src" / "lib",
        build_dir / "_deps" / "zstd-src" / "lib",
        build_dir / "_deps" / "glaze-src" / "include",
    ]
    for candidate in candidates:
        if candidate.is_dir():
            include_dirs.append(str(candidate.resolve()))

    # Also scan the _deps directory for any zlib or zlib-ng source/build folders
    # and add common include/lib locations. This helps when FetchContent or
    # packaged zlib-ng places headers in non-standard subfolders.
    deps_dir = build_dir / "_deps"
    if deps_dir.is_dir():
        for entry in deps_dir.iterdir():
            name = entry.name.lower()
            if not name.startswith("zlib"):
                continue
            # prefer include/, then lib/, then the directory itself
            for sub in ("include", "lib", ""):
                candidate = entry / sub if sub else entry
                if candidate.is_dir():
                    include_dirs.append(str(candidate.resolve()))
                    break
    return include_dirs


def read_defines() -> List[str]:
    version_file = Path("VERSION")
    version = version_file.read_text().strip()
    if not version:
        return []
    return [f'-DAERONET_VERSION_STR="{version}"']


def detect_sanitizer_flags(build_dir: Path) -> List[str]:
    cache_file = build_dir / "CMakeCache.txt"
    if not cache_file.is_file():
        return []
    cache_content = cache_file.read_text()
    flags: List[str] = []

    def append_flag(flag: str) -> None:
        if flag not in flags:
            flags.append(flag)

    if "AERONET_ENABLE_ASAN:BOOL=ON" in cache_content:
        for flag in [
            "-fsanitize=address",
            "-fsanitize=undefined",
            "-fsanitize=float-divide-by-zero",
            "-fno-sanitize-recover",
        ]:
            append_flag(flag)

    for match in re.findall(r"-fsanitize=[^\s\"]+", cache_content):
        append_flag(match)

    return flags


def read_cache_entry(build_dir: Path, key: str) -> Optional[str]:
    cache_file = build_dir / "CMakeCache.txt"
    if not cache_file.is_file():
        return None
    prefix = f"{key}:"
    for line in cache_file.read_text().splitlines():
        if not line.startswith(prefix):
            continue
        _, value = line.split("=", 1)
        return value
    return None


MAIN_FUNCTION_PATTERN = re.compile(r"\bint\s+main\s*\(")
IMPORT_PATTERN = re.compile(r"^\s*import\s+")
INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*(<[^>]+>|"[^"]+")')


def extract_snippets(source: Path, first_index: int) -> List[Snippet]:
    blocks: List[Tuple[int, List[str]]] = []
    current: List[str] = []
    in_block = False
    block_start_line = 0

    for line_number, line in enumerate(source.read_text().splitlines(), start=1):
        if line.startswith("```"):
            tag = line[3:].strip().lower()
            if in_block:
                blocks.append((block_start_line, current))
                current = []
                in_block = False
                continue
            if tag in LANGUAGES:
                in_block = True
                block_start_line = line_number + 1
            continue
        if in_block:
            current.append(line)

    snippets: List[Snippet] = []
    for index, (source_line, lines) in enumerate(blocks, start=first_index):
        snippets.append(
            Snippet(
                index=index,
                source=source,
                source_line=source_line,
                lines=lines,
                needs_std_module=any(
                    re.search(r"^\s*import\s+std\s*;", line) for line in lines
                ),
                needs_aeronet_module=any(
                    re.search(r"^\s*import\s+aeronet\s*;", line)
                    for line in lines
                ),
            )
        )
    return snippets


def is_include_line(line: str) -> bool:
    return line.strip().startswith("#include")


def source_line_directive(source: Path, line_number: int) -> str:
    source_name = source.resolve().as_posix().replace('"', '\\"')
    return f'#line {line_number} "{source_name}"'


def include_key(line: str) -> str:
    match = INCLUDE_PATTERN.match(line)
    return match.group(1) if match else line.strip()


def write_source_line(
    destination: List[str], source: Path, line_number: int, line: str
) -> None:
    destination.extend([source_line_directive(source, line_number), line])


def write_batch_source(batch: SnippetBatch) -> None:
    """Write one translation unit whose snippets retain independent C++ scopes."""
    imports: List[Tuple[Snippet, int, str]] = []
    includes: List[Tuple[Snippet, int, str]] = []
    include_keys = set()
    has_umbrella_include = False

    for snippet in batch.snippets:
        for offset, line in enumerate(snippet.lines):
            line_number = snippet.source_line + offset
            if IMPORT_PATTERN.match(line):
                imports.append((snippet, line_number, line))
                continue
            if not is_include_line(line):
                continue
            key = include_key(line)
            if key in include_keys:
                continue
            include_keys.add(key)
            has_umbrella_include |= key == "<aeronet/aeronet.hpp>"
            includes.append((snippet, line_number, line))

    lines = [
        "// Generated by scripts/verify-md-code.py. Do not edit.",
        f"// Batch contains {len(batch.snippets)} independently-scoped snippets.",
    ]
    seen_imports = set()
    for snippet, line_number, line in imports:
        if line in seen_imports:
            continue
        seen_imports.add(line)
        write_source_line(lines, snippet.source, line_number, line)

    if (
        not batch.needs_std_module
        and not batch.needs_aeronet_module
        and not has_umbrella_include
    ):
        lines.append("#include <aeronet/aeronet.hpp>")
    for snippet, line_number, line in includes:
        write_source_line(lines, snippet.source, line_number, line)

    for snippet in batch.snippets:
        body_lines = [
            (snippet.source_line + offset, line)
            for offset, line in enumerate(snippet.lines)
            if not is_include_line(line) and not IMPORT_PATTERN.match(line)
        ]
        lines.append(f"namespace aeronet_doc_snippet_{snippet.index} {{")
        if not snippet.needs_std_module and not snippet.needs_aeronet_module:
            lines.append("using namespace aeronet;")
        if MAIN_FUNCTION_PATTERN.search("\n".join(line for _, line in body_lines)):
            for line_number, line in body_lines:
                renamed_line = MAIN_FUNCTION_PATTERN.sub(
                    f"int aeronet_doc_snippet_main_{snippet.index}(", line
                )
                write_source_line(lines, snippet.source, line_number, renamed_line)
        else:
            lines.append("void Verify() {")
            for line_number, line in body_lines:
                indented_line = f"  {line}" if line else ""
                write_source_line(lines, snippet.source, line_number, indented_line)
            lines.append("}")
        lines.extend(["}", ""])

    lines.extend(["int main() {", "  return 0;", "}"])
    batch.path.write_text("\n".join(lines) + "\n")


def make_snippet_batches(
    snippets: Sequence[Snippet], snippets_per_batch: int, target_dir: Path
) -> List[SnippetBatch]:
    grouped: dict[Tuple[bool, bool], List[Snippet]] = {}
    for snippet in snippets:
        key = (snippet.needs_std_module, snippet.needs_aeronet_module)
        grouped.setdefault(key, []).append(snippet)

    batches: List[SnippetBatch] = []
    for (needs_std_module, needs_aeronet_module), group in grouped.items():
        for start in range(0, len(group), snippets_per_batch):
            batch_snippets = group[start : start + snippets_per_batch]
            batch_index = len(batches) + 1
            path = target_dir / f"doc_snippet_batch_{batch_index:03d}.cpp"
            batch = SnippetBatch(
                snippets=batch_snippets,
                path=path,
                target=f"doc_snippet_batch_{batch_index:03d}",
                rel_source=path.relative_to(target_dir),
                needs_std_module=needs_std_module,
                needs_aeronet_module=needs_aeronet_module,
            )
            write_batch_source(batch)
            batches.append(batch)
    return batches


def locate_libraries(build_dir: Path) -> List[str]:
    lib_dir = build_dir / "lib"
    if not lib_dir.is_dir():
        print(
            f"Build dir {lib_dir} not found; invoking cmake --build for target aeronet_server",
            file=sys.stderr,
        )
        try:
            subprocess.run(
                ["cmake", "--build", ".", "--target", "aeronet_server", "--parallel"],
                cwd=str(build_dir),
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except subprocess.CalledProcessError:
            print("cmake --build failed; continuing with what we have", file=sys.stderr)

    # The HTTP client lives in a standalone library (aeronet_client) that is NOT a dependency of the
    # main `aeronet` target, so building only `aeronet` (or an examples-only tree that never references
    # the client) never produces libaeronet_client.a. README/FEATURES snippets use HttpClient, so make a
    # best-effort attempt to build the client library too. This is a no-op when the target is already
    # built, and is tolerated when the target is absent (e.g. AERONET_ENABLE_HTTP_CLIENT=OFF).
    try:
        subprocess.run(
            ["cmake", "--build", ".", "--target", "aeronet_client", "--parallel"],
            cwd=str(build_dir),
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except subprocess.CalledProcessError:
        print(
            "cmake --build for aeronet_client failed or target absent; continuing",
            file=sys.stderr,
        )

    # The JWT module (aeronet_jwt) is likewise a standalone library that is not a dependency of the
    # main `aeronet` target, so build it best-effort for the README/FEATURES JWT snippets. No-op when
    # already built, tolerated when absent (AERONET_ENABLE_JWT=OFF).
    try:
        subprocess.run(
            ["cmake", "--build", ".", "--target", "aeronet_jwt", "--parallel"],
            cwd=str(build_dir),
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except subprocess.CalledProcessError:
        print(
            "cmake --build for aeronet_jwt failed or target absent; continuing",
            file=sys.stderr,
        )

    libs: List[str] = []
    primary_path = build_dir / "aeronet" / "main" / "libaeronet.a"
    if primary_path.is_file():
        libs.append(str(primary_path))

    seen = set(libs)
    candidates = sorted(build_dir.rglob("libaeronet*.a"))
    for candidate in candidates:
        candidate_str = str(candidate)
        if candidate_str in seen:
            continue
        libs.append(candidate_str)
        seen.add(candidate_str)

    if not libs:
        print(f"aeronet library not found under {build_dir}", file=sys.stderr)

    return libs


def locate_dependency_libraries(build_dir: Path) -> Tuple[List[str], List[str]]:
    libs: List[str] = []
    fallback_flags: List[str] = []
    seen = set()
    for pattern, flag in DEPENDENCY_LIBRARY_PATTERNS:
        matches = sorted(build_dir.rglob(pattern))
        if not matches:
            fallback_flags.append(flag)
            continue
        for candidate in matches:
            candidate_str = str(candidate)
            if candidate_str in seen:
                continue
            libs.append(candidate_str)
            seen.add(candidate_str)
    return libs, fallback_flags


def resolve_fallback_libs(
    build_dir: Path, fallback_flags: List[str]
) -> Tuple[List[str], List[str]]:
    """Try to resolve fallback `-lname` flags into absolute library paths under build_dir.

    Prefer static archives (`.a`) when present, fall back to shared objects (`.so`).

    Returns:
      A tuple (resolved_libs, unresolved_flags) where resolved_libs are absolute paths
      to library files found under build_dir and unresolved_flags are `-l...` flags
      that could not be located.
    """
    resolved: List[str] = []
    unresolved: List[str] = []
    for flag in fallback_flags:
        if not flag.startswith("-l"):
            unresolved.append(flag)
            continue
        name = flag[2:]
        # search for lib{name}*.a first, then .so
        patterns = [f"lib{name}*.a", f"lib{name}*.so"]
        found: List[str] = []
        for pat in patterns:
            for candidate in build_dir.rglob(pat):
                if candidate.is_file():
                    found.append(str(candidate))
            if found:
                break
        # If not found, try some common alternate zlib-ng / zlib names
        if not found and ("zlib" in name or "z" == name or "z-ng" in name):
            # Prefer zlib-ng libraries first, then fall back to classic zlib
            alt_patterns = [
                "libz-ng*.a",
                "libz-ng*.so*",
                "libz*.a",
                "libz*.so*",
                "libzlib*.a",
                "libzlib*.so*",
            ]
            for pat in alt_patterns:
                for candidate in build_dir.rglob(pat):
                    if candidate.is_file():
                        found.append(str(candidate))
                if found:
                    break
        if found:
            # prefer the first match (sorted by rglob order); prefer .a so patterns already ordered
            resolved.append(found[0])
        else:
            unresolved.append(flag)
    return resolved, unresolved


def read_cmake_feature_defines(build_dir: Path) -> List[str]:
    cache_file = build_dir / "CMakeCache.txt"
    if not cache_file.is_file():
        return []
    flags: List[str] = []
    for line in cache_file.read_text().splitlines():
        if not line.startswith("AERONET_ENABLE_"):
            continue
        if ":BOOL=ON" not in line:
            continue
        name = line.split(":BOOL=")[0]
        flags.append(f"-D{name}=1")
    return flags


def read_cmake_platform_defines(build_dir: Path) -> List[str]:
    system_name = read_cache_entry(build_dir, "CMAKE_SYSTEM_NAME")
    if not system_name:
        system_name = platform.system()
    normalized = system_name.lower()

    if normalized.startswith("windows"):
        return ["-DNOMINMAX=1", "-DAERONET_WINDOWS=1"]
    if normalized.startswith("darwin"):
        return ["-DAERONET_POSIX=1", "-DAERONET_MACOS=1"]
    if normalized.startswith("linux"):
        return ["-DAERONET_POSIX=1", "-DAERONET_LINUX=1"]
    return []


def has_platform_define(compile_definitions: Sequence[str]) -> bool:
    platform_defines = {
        "AERONET_LINUX",
        "AERONET_MACOS",
        "AERONET_WINDOWS",
    }
    for define in compile_definitions:
        name = define.split("=", 1)[0]
        if name in platform_defines:
            return True
    return False


def dump_output(output: str, limit: int = 200) -> None:
    lines = output.splitlines()
    for line in lines[:limit]:
        print(line, file=sys.stderr)
    if len(lines) > limit:
        print("[output truncated]", file=sys.stderr)


def normalize_define(flag: str) -> str:
    return flag[2:] if flag.startswith("-D") else flag


def load_target_usage(
    build_dir: Path, target_name: str = "aeronet"
) -> Optional[TargetUsage]:
    reply_dir = build_dir / ".cmake" / "api" / "v1" / "reply"
    if not reply_dir.is_dir():
        return None
    pattern = f"target-{target_name}-*.json"
    matches = sorted(
        reply_dir.glob(pattern), key=lambda path: path.stat().st_mtime, reverse=True
    )
    if not matches:
        return None
    data = json.loads(matches[0].read_text())
    includes: List[str] = []
    compile_defs: List[str] = []
    compile_opts: List[str] = []
    for group in data.get("compileGroups", []):
        for include in group.get("includes", []):
            path = include.get("path")
            if not path:
                continue
            cmake_path = Path(path)
            if not cmake_path.is_absolute():
                cmake_path = (build_dir / cmake_path).resolve()
            includes.append(str(cmake_path))
        for define in group.get("defines", []):
            value = define.get("define")
            if value:
                compile_defs.append(value)
        for fragment in group.get("compileCommandFragments", []):
            frag = fragment.get("fragment", "").strip()
            if not frag:
                continue
            compile_opts.extend(shlex.split(frag))
    return TargetUsage(
        include_dirs=dedupe_preserve(includes),
        compile_definitions=dedupe_preserve(compile_defs),
        compile_options=dedupe_preserve(compile_opts),
    )


def detect_cxx_compiler(build_dir: Path) -> str:
    """Read CMAKE_CXX_COMPILER from CMakeCache.txt, fall back to CXX env or 'c++'."""
    cached = read_cache_entry(build_dir, "CMAKE_CXX_COMPILER")
    if cached:
        return cached
    return os.environ.get("CXX", "c++")


def uses_libcxx(build_dir: Path) -> bool:
    """Return True if the aeronet build was configured with -stdlib=libc++."""
    cache_file = build_dir / "CMakeCache.txt"
    if not cache_file.is_file():
        return False
    content = cache_file.read_text()
    return "-stdlib=libc++" in content


def build_std_module(build_dir: Path, cxx: str) -> Optional[Path]:
    """Build the C++23 std module and return path to std.pcm if successful.

    The std module (import std;) requires libc++.  If the detected compiler
    does not appear to be Clang or libc++ is unavailable, this returns None.
    """
    std_pcm = build_dir / "std.pcm"
    if std_pcm.exists():
        return std_pcm

    # Try to find a version suffix from the compiler name (e.g. clang++-21 -> 21)
    ver_match = re.search(r"(\d+)$", cxx)
    ver = ver_match.group(1) if ver_match else ""

    # Common locations for std.cppm with libc++
    std_cppm_locations = [
        Path("/usr/lib/libc++/v1/std.cppm"),
        Path("/usr/include/c++/v1/std.cppm.in"),
    ]
    if ver:
        std_cppm_locations.insert(
            0, Path(f"/usr/lib/llvm-{ver}/lib/clang/{ver}/share/std.cppm")
        )

    std_cppm = None
    for location in std_cppm_locations:
        if location.exists():
            std_cppm = location
            break

    if not std_cppm:
        print("Warning: Could not find std.cppm for module build", file=sys.stderr)
        return None

    print(f"Building std.pcm module from {std_cppm}")

    cmd = [
        cxx,
        "-std=gnu++23",
        "-stdlib=libc++",
        "--precompile",
        "-o", str(std_pcm),
        str(std_cppm),
        "-Wno-reserved-module-identifier",
    ]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("Failed to build std.pcm module", file=sys.stderr)
        if result.stderr:
            dump_output(result.stderr)
        return None

    print(f"Successfully built std.pcm at {std_pcm}")
    return std_pcm


@dataclass
class ModulePaths:
    pcm: Path   # Binary Module Interface (.pcm)
    obj: Path   # Compiled object file (.o)


def build_aeronet_module(
    build_dir: Path,
    include_dirs: List[str],
    compile_definitions: List[str],
    cxx: str,
    libcxx: bool = False,
    std_pcm_path: Optional[Path] = None,
) -> Optional[ModulePaths]:
    """Build the aeronet C++ module and return paths to aeronet.pcm and aeronet.o."""
    aeronet_pcm = build_dir / "aeronet.pcm"
    aeronet_obj = build_dir / "aeronet.o"
    if aeronet_pcm.exists() and aeronet_obj.exists():
        return ModulePaths(pcm=aeronet_pcm, obj=aeronet_obj)

    aeronet_cppm = Path("modules/aeronet.cppm")
    if not aeronet_cppm.exists():
        print(f"Warning: Could not find {aeronet_cppm} for module build", file=sys.stderr)
        return None

    print(f"Building aeronet.pcm module from {aeronet_cppm}")

    cmd = [
        cxx,
        "-std=gnu++23",
        "--precompile",
        "-o", str(aeronet_pcm),
        str(aeronet_cppm),
    ]

    if libcxx:
        cmd.append("-stdlib=libc++")

    for inc_dir in include_dirs:
        cmd.extend(["-I", inc_dir])

    for define in compile_definitions:
        if define.startswith("-D"):
            cmd.append(define)
        else:
            cmd.append(f"-D{define}")

    if std_pcm_path:
        cmd.append(f"-fmodule-file=std={std_pcm_path}")

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("Failed to build aeronet.pcm module", file=sys.stderr)
        if result.stderr:
            dump_output(result.stderr)
        return None

    # Compile the BMI to an object file (contains the module initializer)
    obj_cmd = [cxx, "-std=gnu++23", "-c", str(aeronet_pcm), "-o", str(aeronet_obj)]
    if libcxx:
        obj_cmd.append("-stdlib=libc++")
    result = subprocess.run(obj_cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("Failed to compile aeronet.pcm to object file", file=sys.stderr)
        if result.stderr:
            dump_output(result.stderr)
        return None

    print(f"Successfully built aeronet module at {aeronet_pcm}")
    return ModulePaths(pcm=aeronet_pcm, obj=aeronet_obj)


def cmake_escape(value: str) -> str:
    return value.replace("\\", "/").replace('"', '\\"')


def write_cmake_project(
    project_dir: Path,
    snippet_batches: List[SnippetBatch],
    include_dirs: List[str],
    compile_definitions: List[str],
    compile_options: List[str],
    libs: List[str],
    dependency_libs: List[str],
    dependency_link_flags: List[str],
    system_link_flags: List[str],
    sanitize_flags: List[str],
    link_mode: bool,
    std_pcm_path: Optional[Path] = None,
    aeronet_module: Optional[ModulePaths] = None,
    libcxx: bool = False,
) -> Path:
    cmakelists = project_dir / "CMakeLists.txt"
    env_target = "aeronet_doc_env"
    lines: List[str] = [
        "cmake_minimum_required(VERSION 3.23)",
        "project(aeronet_doc_snippets LANGUAGES CXX)",
        "set(CMAKE_CXX_STANDARD 23)",
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)",
        f"add_library({env_target} INTERFACE)",
    ]

    if include_dirs:
        lines.append(f"target_include_directories({env_target} INTERFACE")
        for directory in include_dirs:
            lines.append(f'  "{cmake_escape(directory)}"')
        lines.append(")")

    if compile_definitions:
        lines.append(f"target_compile_definitions({env_target} INTERFACE")
        for define in compile_definitions:
            lines.append(f"  {define}")
        lines.append(")")

    if compile_options:
        lines.append(f"target_compile_options({env_target} INTERFACE")
        for option in compile_options:
            lines.append(f"  {option}")
        lines.append(")")

    if link_mode:
        link_entries = (
            libs + dependency_libs + dependency_link_flags + system_link_flags
        )
        if link_entries:
            use_link_group = len(link_entries) > 1
            lines.append(f"target_link_libraries({env_target} INTERFACE")
            if use_link_group:
                lines.append("  -Wl,--start-group")
            for entry in link_entries:
                if entry.startswith("-"):
                    lines.append(f"  {entry}")
                else:
                    lines.append(f'  "{cmake_escape(entry)}"')
            if use_link_group:
                lines.append("  -Wl,--end-group")
            lines.append(")")
        if sanitize_flags:
            lines.append(f"target_link_options({env_target} INTERFACE")
            for flag in sanitize_flags:
                lines.append(f"  {flag}")
            lines.append(")")

    for target in snippet_batches:
        source = cmake_escape(target.rel_source.as_posix())
        if link_mode:
            lines.append(f'add_executable({target.target} "{source}")')
        else:
            lines.append(f'add_library({target.target} OBJECT "{source}")')
        lines.append(f"target_link_libraries({target.target} PRIVATE {env_target})")
        
        module_flags: List[str] = []
        if target.needs_std_module and std_pcm_path:
            std_pcm = cmake_escape(str(std_pcm_path))
            module_flags.append(f'-fmodule-file=std={std_pcm}')
        if target.needs_aeronet_module and aeronet_module:
            aeronet_pcm = cmake_escape(str(aeronet_module.pcm))
            module_flags.append(f'-fmodule-file=aeronet={aeronet_pcm}')

        if module_flags:
            lines.append(f"target_compile_options({target.target} PRIVATE")
            if libcxx or target.needs_std_module:
                lines.append("  -stdlib=libc++")
            for flag in module_flags:
                lines.append(f'  "{flag}"')
            lines.append(")")
            if libcxx or target.needs_std_module:
                lines.append(f"target_link_options({target.target} PRIVATE -stdlib=libc++ -lc++)")

        # Link the compiled module object so the module initializer is defined
        if link_mode and target.needs_aeronet_module and aeronet_module:
            obj_escaped = cmake_escape(str(aeronet_module.obj))
            lines.append(f'target_link_libraries({target.target} PRIVATE "{obj_escaped}")')

    cmakelists.write_text("\n".join(lines) + "\n")
    return cmakelists


def configure_cmake_project(
    cmake_bin: str,
    source_dir: Path,
    build_dir: Path,
    generator: Optional[str],
    build_type: Optional[str],
    cxx_compiler: Optional[str] = None,
) -> bool:
    cmd = [cmake_bin, "-S", str(source_dir), "-B", str(build_dir)]
    if generator:
        cmd.extend(["-G", generator])
    if build_type:
        cmd.append(f"-DCMAKE_BUILD_TYPE={build_type}")
    if cxx_compiler:
        cmd.append(f"-DCMAKE_CXX_COMPILER={cxx_compiler}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode == 0:
        return True
    print("CMake configure failed", file=sys.stderr)
    if result.stderr:
        dump_output(result.stderr)
    if result.stdout:
        dump_output(result.stdout)
    return False


def build_cmake_target(cmake_bin: str, build_dir: Path, target: str) -> bool:
    cmd = [cmake_bin, "--build", str(build_dir), "--target", target]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode == 0:
        return True
    print(f"Build failed for target {target}", file=sys.stderr)
    if result.stderr:
        dump_output(result.stderr)
    if result.stdout:
        dump_output(result.stdout)
    return False


def batch_locations(batch: SnippetBatch) -> str:
    return ", ".join(
        f"#{snippet.index} {snippet.source}:{snippet.source_line}"
        for snippet in batch.snippets
    )


def build_snippet_batch_task(
    index: int,
    batch: SnippetBatch,
    cmake_bin: str,
    build_dir: Path,
) -> bool:
    print(
        f"Building batch #{index} ({len(batch.snippets)} snippets: "
        f"{batch_locations(batch)})"
    )
    return build_cmake_target(cmake_bin, build_dir, batch.target)


def main() -> None:
    args = parse_args()
    sources: List[Path] = []
    for file in args.files:
        source = Path(file)
        if not source.is_file():
            print(f"Skipping missing file: {file}", file=sys.stderr)
            continue
        sources.append(source)

    checked_links, link_failures = find_internal_link_failures(sources)
    if link_failures:
        print_internal_link_failures(link_failures)
        print(
            f"Checked {checked_links} internal Markdown links, "
            f"{len(link_failures)} failures",
            file=sys.stderr,
        )
        sys.exit(1)

    build_dir = Path(args.build_dir).resolve()
    usage = load_target_usage(build_dir)

    include_dirs: List[str] = []
    compile_definitions: List[str] = []
    compile_options: List[str] = []

    if usage:
        include_dirs.extend(usage.include_dirs)
        compile_definitions.extend(usage.compile_definitions)
        compile_options.extend(usage.compile_options)
    else:
        print(
            "CMake target metadata missing; falling back to heuristic include directories",
            file=sys.stderr,
        )
        include_dirs.extend(fallback_include_dirs())
        include_dirs.extend(locate_dependency_include_dirs(build_dir))
        fallback_defines = read_defines()
        fallback_defines.extend(read_cmake_feature_defines(build_dir))
        compile_definitions.extend(normalize_define(flag) for flag in fallback_defines)

    if not has_platform_define(compile_definitions):
        compile_definitions.extend(
            normalize_define(flag) for flag in read_cmake_platform_defines(build_dir)
        )

    include_dirs.extend(gather_user_include_dirs(args.include))
    # The HTTP client lives in its own module whose public include tree is not pulled in by the
    # main `aeronet` target metadata; add it explicitly so documentation snippets using
    # <aeronet/http-client.hpp> compile (the matching libaeronet_client.a is linked via rglob).
    client_include = Path("aeronet/client/include")
    if client_include.is_dir():
        include_dirs.append(str(client_include.resolve()))
    include_dirs = dedupe_preserve(include_dirs)
    compile_definitions = dedupe_preserve(compile_definitions)

    sanitize_flags = detect_sanitizer_flags(build_dir)
    for flag in sanitize_flags:
        if flag not in compile_options:
            compile_options.append(flag)
    compile_options = dedupe_preserve(compile_options)

    libs: List[str] = []
    dependency_libs: List[str] = []
    dependency_link_flags: List[str] = []
    if args.link:
        libs = locate_libraries(build_dir)
        dependency_libs, dependency_link_flags = locate_dependency_libraries(build_dir)
        # Resolve any fallback `-l...` flags into concrete library paths when possible;
        # only keep unresolved flags (for system libs) in dependency_link_flags.
        if dependency_link_flags:
            resolved, dependency_link_flags = resolve_fallback_libs(
                build_dir, dependency_link_flags
            )
            dependency_libs.extend(resolved)

    snippets: List[Snippet] = []
    with tempfile.TemporaryDirectory(prefix="aeronet-verify-md-") as tmpdir:
        tmp_path = Path(tmpdir)
        for source in sources:
            snippets.extend(extract_snippets(source, len(snippets) + 1))

        if not snippets:
            print(
                f"Checked 0 code snippets and {checked_links} internal Markdown links, "
                "0 failures"
            )
            return

        cxx = detect_cxx_compiler(build_dir)
        libcxx = uses_libcxx(build_dir)

        needs_std = any(snippet.needs_std_module for snippet in snippets)
        needs_aeronet = any(snippet.needs_aeronet_module for snippet in snippets)

        std_pcm_path = None
        if needs_std:
            std_pcm_path = build_std_module(tmp_path, cxx)
            if not std_pcm_path:
                print("Warning: Some snippets need std module but build failed", file=sys.stderr)

        aeronet_module = None
        if needs_aeronet:
            aeronet_module = build_aeronet_module(
                tmp_path,
                include_dirs,
                compile_definitions,
                cxx,
                libcxx,
                std_pcm_path,
            )
            if not aeronet_module:
                print("Warning: Some snippets need aeronet module but build failed", file=sys.stderr)

        snippet_batches = make_snippet_batches(
            snippets, args.snippets_per_batch, tmp_path
        )
        print(
            f"Building {len(snippets)} snippets in {len(snippet_batches)} batches "
            f"(up to {args.snippets_per_batch} snippets per batch)"
        )

        write_cmake_project(
            tmp_path,
            snippet_batches,
            include_dirs,
            compile_definitions,
            compile_options,
            libs,
            dependency_libs,
            dependency_link_flags,
            SYSTEM_LINK_FLAGS,
            sanitize_flags,
            args.link,
            std_pcm_path,
            aeronet_module,
            libcxx,
        )

        build_type = args.cmake_build_type or read_cache_entry(
            build_dir, "CMAKE_BUILD_TYPE"
        )
        # Only pass cxx_compiler when snippets use modules (must match the
        # compiler that produced the .pcm files).
        snippet_cxx = cxx if (needs_std or needs_aeronet) else None
        cmake_build_dir = tmp_path / "cmake-build"
        if not configure_cmake_project(
            args.cmake,
            tmp_path,
            cmake_build_dir,
            args.generator,
            build_type,
            snippet_cxx,
        ):
            sys.exit(1)

        failures = 0
        job_limit = args.jobs if args.jobs and args.jobs > 1 else None
        if job_limit:
            future_to_idx: dict[concurrent.futures.Future, int] = {}
            with concurrent.futures.ThreadPoolExecutor(
                max_workers=job_limit
            ) as executor:
                for idx, target in enumerate(snippet_batches, start=1):
                    future = executor.submit(
                        build_snippet_batch_task,
                        idx,
                        target,
                        args.cmake,
                        cmake_build_dir,
                    )
                    future_to_idx[future] = idx
                for future in concurrent.futures.as_completed(future_to_idx):
                    idx = future_to_idx[future]
                    try:
                        success = future.result()
                    except Exception as exc:
                        print(
                            f"Unexpected error building batch #{idx}: {exc}",
                            file=sys.stderr,
                        )
                        failures += 1
                        continue
                    if not success:
                        failures += 1
        else:
            for idx, target in enumerate(snippet_batches, start=1):
                if not build_snippet_batch_task(idx, target, args.cmake, cmake_build_dir):
                    failures += 1

    print(
        f"Checked {len(snippets)} code snippets and {checked_links} internal "
        f"Markdown links in {len(snippet_batches)} batches, {failures} failures"
    )
    sys.exit(failures)


if __name__ == "__main__":
    main()
