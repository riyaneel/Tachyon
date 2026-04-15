#!/usr/bin/env python3

import re
import subprocess
import sys
from collections import defaultdict
from datetime import date
from pathlib import Path

CHANGELOG_PATH = Path(__file__).parent.parent.parent / "CHANGELOG"

SECTION_ORDER = ["feat", "fix", "perf", "refactor", "build", "ci", "test", "docs", "chore"]

SECTION_LABELS = {
    "feat": "Features",
    "fix": "Bug Fixes",
    "perf": "Performance Improvements",
    "refactor": "Code Refactoring",
    "build": "Build System",
    "ci": "Continuous Integration",
    "test": "Tests",
    "docs": "Documentation",
    "chore": "Chores",
}

TYPE_ALIASES = {
    "bindings": "feat",
    "bench": "perf",
    "style": "refactor"
}

COMMIT_RE = re.compile(
    r"^(?P<type>\w+)"
    r"(?:\((?P<scope>[^)]+)\))?"
    r"(?P<breaking>!)?"
    r":\s+(?P<msg>.+)$"
)


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def get_tags() -> list[str]:
    out = git("tag", "--sort=version:refname")
    return [t for t in out.splitlines() if t]


def get_commits(from_ref: str | None, to_ref: str) -> list[str]:
    ref_range = f"{from_ref}..{to_ref}" if from_ref else to_ref
    out = git("log", "--format=%h %s", "--no-merges", ref_range)
    return [line for line in out.splitlines() if line]


def parse_commits(subjects: list[str]) -> dict[str, list[str]]:
    sections: dict[str, list[str]] = defaultdict(list)
    for subject in subjects:
        parts = subject.split(" ", 1)
        if len(parts) < 2:
            continue

        hash_val, subject = parts[0], parts[1]
        m = COMMIT_RE.match(subject)
        if not m:
            sections["other"].append(f"- `{hash_val}` | {subject.capitalize()}")
            continue

        ctype = m.group("type").lower()
        ctype = TYPE_ALIASES.get(ctype, ctype)
        scope = m.group("scope")
        msg = m.group("msg")
        breaking = m.group("breaking")
        msg = msg[0].upper() + msg[1:] if msg else ""
        scope_tag = f"**{scope}**: " if scope else ""
        prefix = "⚠️ **BREAKING**: " if breaking else ""
        entry = f"- `{hash_val}` | {prefix}{scope_tag}{msg}"
        sections[ctype].append(entry)

    return sections


def render_section(tag: str, sections: dict[str, list[str]]) -> str:
    today = date.today().isoformat()
    lines = [f"## [{tag}] | {today}", ""]

    all_types = SECTION_ORDER + [t for t in sections if t not in SECTION_ORDER]

    for ctype in all_types:
        entries = sections.get(ctype)
        if not entries:
            continue
        label = SECTION_LABELS.get(ctype, ctype)
        lines.append(f"### {label}")
        lines.append("")
        lines.extend(entries)
        lines.append("")

    return "\n".join(lines)


def prepend_to_changelog(section: str) -> None:
    header = "# Changelog\n\n"
    existing = ""
    if CHANGELOG_PATH.exists():
        existing = CHANGELOG_PATH.read_text(encoding="utf-8")
        if existing.startswith(header):
            existing = existing[len(header):]

    CHANGELOG_PATH.write_text(header + section + "\n" + existing)
    print(f"Updated {CHANGELOG_PATH}")


def main() -> None:
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <tag> [<previous_tag>]", file=sys.stderr)
        sys.exit(1)

    tag = sys.argv[1]

    if len(sys.argv) >= 3:
        prev_tag: str | None = sys.argv[2]
    else:
        tags = get_tags()
        try:
            idx = tags.index(tag)
        except ValueError:
            print(f"Tag '{tag}' not found in repo.", file=sys.stderr)
            sys.exit(1)
        prev_tag = tags[idx - 1] if idx > 0 else None

    print(f"Generating changelog for {tag} (since {prev_tag or 'beginning'})")

    subjects = get_commits(prev_tag, tag)
    if not subjects:
        print("No commits found: nothing to write.")
        return

    sections = parse_commits(subjects)
    section = render_section(tag, sections)
    prepend_to_changelog(section)


if __name__ == "__main__":
    main()
