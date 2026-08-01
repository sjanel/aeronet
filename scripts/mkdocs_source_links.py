"""Rewrite local repository source links to GitHub during MkDocs builds."""

import re
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit


MARKDOWN_LINK_PATTERN = re.compile(r"(?<![!\\])\[([^]\n]+)\]\(([^)\n]+)\)")
MARKDOWN_FENCE_PATTERN = re.compile(r"^\s*(`{3,}|~{3,})")
INLINE_CODE_PATTERN = re.compile(r"(`+).*?\1")
SOURCE_BRANCH = "main"


def github_source_url(destination: str, source: Path, docs_dir: Path, repo_url: str) -> str | None:
    parsed = urlsplit(destination)
    if parsed.scheme or parsed.netloc or not parsed.path:
        return None

    target = (source.parent / unquote(parsed.path)).resolve()
    try:
        target.relative_to(docs_dir)
        return None
    except ValueError:
        pass

    repo_root = docs_dir.parent
    try:
        repo_path = target.relative_to(repo_root)
    except ValueError:
        return None
    if target.is_file():
        target_kind = "blob"
    elif target.is_dir():
        target_kind = "tree"
    else:
        return None

    url = f"{repo_url.rstrip('/')}/{target_kind}/{SOURCE_BRANCH}/"
    url += quote(repo_path.as_posix(), safe="/")
    if parsed.query:
        url += f"?{parsed.query}"
    if parsed.fragment:
        url += f"#{parsed.fragment}"
    return url


def rewrite_links(line: str, source: Path, docs_dir: Path, repo_url: str) -> str:
    def replace_link(match: re.Match[str]) -> str:
        destination = match.group(2).strip()
        if destination.startswith("<") and destination.endswith(">"):
            destination = destination[1:-1]
        github_url = github_source_url(destination, source, docs_dir, repo_url)
        if not github_url:
            return match.group(0)
        return f"[{match.group(1)}]({github_url})"

    code_spans: list[str] = []

    def mask_code_span(match: re.Match[str]) -> str:
        marker = f"\x00{len(code_spans)}\x00"
        code_spans.append(match.group(0))
        return marker

    rewritten = MARKDOWN_LINK_PATTERN.sub(
        replace_link, INLINE_CODE_PATTERN.sub(mask_code_span, line)
    )
    for index, code_span in enumerate(code_spans):
        rewritten = rewritten.replace(f"\x00{index}\x00", code_span)
    return rewritten


def on_page_markdown(markdown: str, *, page, config, files) -> str:
    """Rewrite source links before MkDocs renders and validates the page."""
    del files
    source = Path(page.file.abs_src_path)
    docs_dir = Path(config["docs_dir"]).resolve()
    repo_url = config["repo_url"]
    rewritten_lines: list[str] = []
    fence = ""

    for line in markdown.splitlines(keepends=True):
        fence_match = MARKDOWN_FENCE_PATTERN.match(line)
        if fence_match:
            delimiter = fence_match.group(1)
            if fence:
                if delimiter[0] == fence[0] and len(delimiter) >= len(fence):
                    fence = ""
            else:
                fence = delimiter
            rewritten_lines.append(line)
            continue
        if fence:
            rewritten_lines.append(line)
            continue
        rewritten_lines.append(rewrite_links(line, source, docs_dir, repo_url))

    return "".join(rewritten_lines)
