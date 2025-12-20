#!/usr/bin/env python3
"""Extract and build C/C++ code fences from Markdown documentation definitions."""

import argparse
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Sequence, Tuple


LANGUAGES = {"c", "cpp", "c++"}

DEPENDENCY_LIBRARY_PATTERNS = [
    ("libzstd*.a", "-lzstd"),
    ("libbrotlienc*.a", "-lbrotlienc"),
    ("libbrotlidec*.a", "-lbrotlidec"),
    ("libbrotlicommon*.a", "-lbrotlicommon"),
    # OpenTelemetry libraries - only include those that are actually built.
    # Note: patterns with wildcards (like *_http*.a) can match multiple libs,
    # so we use exact names where possible to avoid duplicates.
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
]

SYSTEM_LINK_FLAGS = [
    "-lssl",
    "-lcrypto",
    "-lz",
    "-lcurl",
    "-lprotobuf",
]


@dataclass
class Snippet:
    source: Path
    path: Path


@dataclass
class SnippetTarget:
    snippet: Snippet
    target: str
    rel_source: Path


@dataclass
class TargetUsage:
    include_dirs: List[str]
    compile_definitions: List[str]
    compile_options: List[str]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compile/link C/C++ fences from Markdown files to detect doc bitrot. "
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
        help="Link each snippet into an executable (default: compile only)",
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
        help="Number of worker threads used to build snippets in parallel",
    )
    parser.add_argument(
        "files",
        nargs="*",
        default=["README.md", "FEATURES.md"],
        help="Markdown files to scan for code snippets",
    )
    return parser.parse_args()


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
        "aeronet/main/include",
        "aeronet/http/include",
        "aeronet/http2/include",
        "aeronet/objects/include",
        "aeronet/tech/include",
        "aeronet/sys/include",
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
        build_dir / "_deps" / "zstd-src" / "lib",
    ]
    for candidate in candidates:
        if candidate.is_dir():
            include_dirs.append(str(candidate.resolve()))
    return include_dirs


def read_defines() -> List[str]:
    version_file = Path("VERSION")
    if not version_file.is_file():
        return []
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


def extract_snippets(source: Path, target_dir: Path) -> List[Snippet]:
    text = source.read_text()
    blocks: List[List[str]] = []
    current: List[str] = []
    in_block = False

    for line in text.splitlines():
        if line.startswith("```"):
            tag = line[3:].strip().lower()
            if in_block:
                blocks.append(current)
                current = []
                in_block = False
                continue
            if tag in LANGUAGES:
                in_block = True
            continue
        if in_block:
            current.append(line)

    sanitized = re.sub(r"[^0-9A-Za-z]+", "_", source.stem)
    snippets: List[Snippet] = []
    for idx, block in enumerate(blocks, start=1):
        dest = target_dir / f"{sanitized}_block_{idx:03d}.cpp"
        has_main = any(re.search(r"\bint\s+main\s*\(", line) for line in block)
        include_lines = [line for line in block if line.strip().startswith("#include")]
        body_lines = [line for line in block if not line.strip().startswith("#include")]
        if not has_main:
            indented = [f"  {line}" if line.strip() else "" for line in body_lines]
            body_lines = ["int main() {", *indented, "  return 0;", "}"]
        final_lines = include_lines + body_lines
        dest.write_text(
            "// extracted from {}\n#include <aeronet/aeronet.hpp>\nusing namespace aeronet;\n{}\n".format(
                source.name, "\n".join(final_lines)
            )
        )
        snippets.append(Snippet(source=source, path=dest))
    return snippets


def locate_libraries(build_dir: Path) -> List[str]:
    lib_dir = build_dir / "lib"
    if not lib_dir.is_dir():
        print(
            f"Build dir {lib_dir} not found; invoking cmake --build for target aeronet",
            file=sys.stderr,
        )
        try:
            subprocess.run(
                ["cmake", "--build", ".", "--target", "aeronet", "--parallel"],
                cwd=str(build_dir),
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except subprocess.CalledProcessError:
            print("cmake --build failed; continuing with what we have", file=sys.stderr)

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


def cmake_escape(value: str) -> str:
    return value.replace("\\", "/").replace('"', '\\"')


def write_cmake_project(
    project_dir: Path,
    snippet_targets: List[SnippetTarget],
    include_dirs: List[str],
    compile_definitions: List[str],
    compile_options: List[str],
    libs: List[str],
    dependency_libs: List[str],
    dependency_link_flags: List[str],
    system_link_flags: List[str],
    sanitize_flags: List[str],
    link_mode: bool,
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

    for target in snippet_targets:
        source = cmake_escape(target.rel_source.as_posix())
        if link_mode:
            lines.append(f'add_executable({target.target} "{source}")')
        else:
            lines.append(f'add_library({target.target} OBJECT "{source}")')
        lines.append(f"target_link_libraries({target.target} PRIVATE {env_target})")

    cmakelists.write_text("\n".join(lines) + "\n")
    return cmakelists


def configure_cmake_project(
    cmake_bin: str,
    source_dir: Path,
    build_dir: Path,
    generator: Optional[str],
    build_type: Optional[str],
) -> bool:
    cmd = [cmake_bin, "-S", str(source_dir), "-B", str(build_dir)]
    if generator:
        cmd.extend(["-G", generator])
    if build_type:
        cmd.append(f"-DCMAKE_BUILD_TYPE={build_type}")
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


def build_snippet_task(
    idx: int,
    snippet_target: SnippetTarget,
    cmake_bin: str,
    build_dir: Path,
) -> bool:
    print(
        f"Building snippet #{idx} from {snippet_target.snippet.source.name} -> {snippet_target.rel_source}"
    )
    return build_cmake_target(cmake_bin, build_dir, snippet_target.target)


def main() -> None:
    args = parse_args()
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

    include_dirs.extend(gather_user_include_dirs(args.include))
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
        for file in args.files:
            source = Path(file)
            if not source.is_file():
                print(f"Skipping missing file: {file}", file=sys.stderr)
                continue
            snippets.extend(extract_snippets(source, tmp_path))

        if not snippets:
            print("Checked 0 code snippets, 0 failures")
            return

        snippet_targets: List[SnippetTarget] = []
        for idx, snippet in enumerate(snippets, start=1):
            rel_source = snippet.path.relative_to(tmp_path)
            snippet_targets.append(
                SnippetTarget(
                    snippet=snippet,
                    target=f"doc_snippet_{idx:03d}",
                    rel_source=rel_source,
                )
            )

        write_cmake_project(
            tmp_path,
            snippet_targets,
            include_dirs,
            compile_definitions,
            compile_options,
            libs,
            dependency_libs,
            dependency_link_flags,
            SYSTEM_LINK_FLAGS,
            sanitize_flags,
            args.link,
        )

        build_type = args.cmake_build_type or read_cache_entry(
            build_dir, "CMAKE_BUILD_TYPE"
        )
        cmake_build_dir = tmp_path / "cmake-build"
        if not configure_cmake_project(
            args.cmake,
            tmp_path,
            cmake_build_dir,
            args.generator,
            build_type,
        ):
            sys.exit(1)

        failures = 0
        job_limit = args.jobs if args.jobs and args.jobs > 1 else None
        if job_limit:
            future_to_idx: dict[concurrent.futures.Future, int] = {}
            with concurrent.futures.ThreadPoolExecutor(
                max_workers=job_limit
            ) as executor:
                for idx, target in enumerate(snippet_targets, start=1):
                    future = executor.submit(
                        build_snippet_task,
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
                            f"Unexpected error building snippet #{idx}: {exc}",
                            file=sys.stderr,
                        )
                        failures += 1
                        continue
                    if not success:
                        failures += 1
        else:
            for idx, target in enumerate(snippet_targets, start=1):
                if not build_snippet_task(idx, target, args.cmake, cmake_build_dir):
                    failures += 1

    print(f"Checked {len(snippet_targets)} code snippets, {failures} failures")
    sys.exit(failures)


if __name__ == "__main__":
    main()
