#!/usr/bin/env python3
"""Claude Code hook: remind the agent to keep the agent docs current.

`snapshot` (UserPromptSubmit) records a hash of every dirty working-tree file.
`check` (Stop) compares against it, so only files changed during this turn
count, whether edited by a tool or a shell command, committed or not. When a
turn adds/removes files or touches a contract/build file in a mapped area and
did not touch that area's guide, it blocks the stop once with a short reminder.
It also runs scripts/check-agent-docs.py when docs or file sets changed.
No LLM call is made here; `stop_hook_active` prevents a second block.
"""
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STATE = ROOT / ".claude" / ".doc-state"

# Changed path prefix -> guide that owns its map. First match wins.
AREAS = [
    ("shared/matp/", "docs/protocol/MATP-AudioTransferProtocol.md"),
    ("client/", "docs/agents/client.md"),
    ("server-core/", "docs/agents/server-core.md"),
    ("server/", "docs/agents/server.md"),
    ("server-gui/", "docs/agents/server.md"),
    ("shared/", "docs/agents/overview.md"),
    ("scripts/", "docs/agents/development.md"),
]
# Edits here can change contracts, wiring, commands or build lists.
CONTRACT = re.compile(
    r"^(shared/matp/"
    r"|server-core/src/control/ControlApi\.cpp"
    r"|server/src/Multiroom\.Server\.Playback/Core/"
    r"|server/src/Multiroom\.Server/ServerHost\.cs"
    r"|scripts/[^/]+$"
    r"|.*(CMakeLists\.txt|CMakePresets\.json|\.csproj|\.props)$)"
)


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout


def dirty():
    """{path: content hash or None if deleted} for files differing from HEAD or untracked."""
    paths = set(git("diff", "--name-only", "HEAD", "-z").split("\0"))
    paths |= set(git("ls-files", "-o", "--exclude-standard", "-z").split("\0"))
    paths.discard("")
    result = {}
    for p in paths:
        f = ROOT / p
        result[p] = hashlib.sha1(f.read_bytes()).hexdigest() if f.is_file() else None
    return result


def state_file(session):
    return STATE / f"{re.sub(r'[^A-Za-z0-9_-]', '', session) or 'default'}.json"


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    event = json.load(sys.stdin)
    snap = state_file(event.get("session_id", ""))

    if mode == "snapshot":
        STATE.mkdir(parents=True, exist_ok=True)
        snap.write_text(json.dumps({"dirty": dirty(), "head": git("rev-parse", "HEAD").strip()}))
        return 0
    if mode != "check" or event.get("stop_hook_active") or not snap.exists():
        return 0

    before = json.loads(snap.read_text())
    now = dirty()
    changed = {p for p in before["dirty"].keys() | now.keys() if before["dirty"].get(p) != now.get(p)}
    # Files committed during the turn leave the dirty set; include them too.
    if before["head"] and before["head"] != git("rev-parse", "HEAD").strip():
        changed |= set(git("diff", "--name-only", before["head"], "HEAD").splitlines())
    if not changed:
        return 0
    # Added, deleted or renamed during this turn: the file's existence changed.
    in_head = set(git("ls-tree", "-r", "--name-only", "-z", before["head"] or "HEAD", "--", *changed).split("\0"))
    structural = {
        p for p in changed
        if (before["dirty"][p] is not None if p in before["dirty"] else p in in_head) != (ROOT / p).is_file()
    }

    hints = {}
    for p in sorted(changed):
        guide = next((g for prefix, g in AREAS if p.startswith(prefix)), None)
        if guide and guide not in changed and (p in structural or CONTRACT.match(p)):
            hints.setdefault(guide, []).append(p)

    problems = []
    if structural or any(p.endswith(".md") for p in changed):
        check = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "check-agent-docs.py")],
            cwd=ROOT, capture_output=True, text=True,
        )
        if check.returncode:
            problems = check.stdout.strip().splitlines()

    if not hints and not problems:
        return 0
    lines = ["Agent docs check before finishing:"]
    for guide, files in hints.items():
        shown = ", ".join(files[:6]) + (" ..." if len(files) > 6 else "")
        lines.append(f"- {guide} may be affected by: {shown}")
    if hints:
        lines.append(
            "Update a guide only if ownership, entry points, contracts, commands or "
            "invariants changed; edit just the affected lines. Otherwise finish without doc changes."
        )
    if problems:
        lines.append("scripts/check-agent-docs.py reported stale references:")
        lines += [f"  {line}" for line in problems[:20]]
    print(json.dumps({"decision": "block", "reason": "\n".join(lines)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
