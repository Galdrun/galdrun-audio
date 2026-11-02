#!/usr/bin/env python3
"""Deterministic checks for the agent docs (AGENTS.md, docs/agents/).

- Relative Markdown links in AGENTS.md, CLAUDE.md and docs/**/*.md resolve.
- Backticked file/directory paths in AGENTS.md and docs/agents/*.md name an
  existing tracked or untracked (non-ignored) file. Paths may be written
  relative to the component, so a suffix match is enough.

Exit code 1 lists every stale reference. Run from anywhere:
    python scripts/check-agent-docs.py
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FILE_EXT = re.compile(
    r"\.(c|cc|cpp|h|hpp|cs|csproj|sln|slnx|js|mjs|ts|css|html|json|md|ps1|sh|py|txt|cmake|"
    r"dts|dtsi|patch|conf|yml|yaml|xml|sql|ini|defaults)$"
)
CODE_EXT = re.compile(r"\.(c|cc|cpp|h|hpp|cs|csproj|js|mjs|ts|css|py|dts|dtsi|patch)$")
OPT_OUT = "<!-- path-check: ignore -->"
LINK = re.compile(r"\]\(([^)\s]+)\)")
CODE = re.compile(r"`([^`\s]+)`")


def repo_files():
    out = subprocess.run(
        ["git", "ls-files", "-c", "-o", "--exclude-standard"],
        cwd=ROOT, capture_output=True, text=True, check=True,
    ).stdout
    files = [line for line in out.splitlines() if line]
    dirs = {"/".join(f.split("/")[:i]) + "/" for f in files for i in range(1, f.count("/") + 1)}
    return files, dirs


def path_like(token):
    """Repository paths only: not absolute/runtime paths, bare extensions,
    single-segment output dirs (`build/`) or bare runtime file names."""
    if any(c in token for c in "*<>{}$|=") or "://" in token or token.startswith(("-", "/", ".")):
        return False
    if "/" not in token:
        return bool(CODE_EXT.search(token))
    if token.endswith("/"):
        return token.count("/") > 1
    return bool(FILE_EXT.search(token))


def exists(token, files, dirs):
    while token.startswith(("./", "../")):
        token = token.split("/", 1)[1]
    # "/x" matches component-relative paths; ".x" matches abbreviations such as
    # `Playback/` for `server/src/Multiroom.Server.Playback/`.
    pool = dirs if token.endswith("/") else files
    return any(p == token or p.endswith(("/" + token, "." + token)) for p in pool) or (ROOT / token).exists()


def ignored(tokens):
    """Generated/output paths that .gitignore excludes are not stale."""
    queries = {t + "x" if t.endswith("/") else t: t for t in tokens}
    out = subprocess.run(
        ["git", "check-ignore", "--no-index", "--stdin"], cwd=ROOT, text=True,
        input="\n".join(queries), capture_output=True,
    ).stdout
    return {queries[line] for line in out.splitlines() if line in queries}


def main():
    files, dirs = repo_files()
    md = [ROOT / "AGENTS.md", ROOT / "CLAUDE.md", *sorted((ROOT / "docs").rglob("*.md"))]
    agent_docs = {ROOT / "AGENTS.md", *(ROOT / "docs" / "agents").glob("*.md")}
    errors, missing = [], []
    for doc in md:
        if not doc.exists():
            continue
        rel = doc.relative_to(ROOT).as_posix()
        for n, line in enumerate(doc.read_text(encoding="utf-8").splitlines(), 1):
            for target in LINK.findall(line):
                target = target.split("#", 1)[0]
                if not target or re.match(r"^[a-z]+:", target):
                    continue
                if not (doc.parent / target).exists():
                    errors.append(f"{rel}:{n}: broken link {target}")
            if doc in agent_docs and OPT_OUT not in line:
                for token in CODE.findall(line):
                    if path_like(token) and not exists(token, files, dirs):
                        missing.append((rel, n, token))
    skip = ignored({token for _, _, token in missing})
    errors += [f"{rel}:{n}: missing path `{token}`" for rel, n, token in missing if token not in skip]
    for e in errors:
        print(e)
    if errors:
        print(f"{len(errors)} stale reference(s); fix the doc or the path.", file=sys.stderr)
        return 1
    print("agent docs OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
