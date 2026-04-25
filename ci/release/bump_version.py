#!/usr/bin/env python3

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


@dataclass
class Entry:
    rel_path: str
    pattern: str
    replacement: str
    flags: int = 0
    max_count: int = 1  # 0 = unlimited


def parse_ver(tag: str) -> str:
    return tag.lstrip("v")


def build_entries(old: str, new: str) -> list[Entry]:
    ov = re.escape(old)
    return [
        # Python
        Entry(
            "bindings/python/pyproject.toml",
            rf'(^\s*version\s*=\s*"){ov}(")',
            rf"\g<1>{new}\g<2>",
            re.MULTILINE,
        ),
        Entry(
            "bindings/python/src/tachyon/__init__.py",
            rf'(__version__\s*=\s*"){ov}(")',
            rf"\g<1>{new}\g<2>",
        ),
        Entry(
            "bindings/python/src/tachyon/__init__.pyi",
            rf'(__version__\s*=\s*"){ov}(")',
            rf"\g<1>{new}\g<2>",
        ),
        # Rust
        Entry(
            "bindings/rust/tachyon-sys/Cargo.toml",
            rf'(^\s*version\s*=\s*"){ov}(")',
            rf"\g<1>{new}\g<2>",
            re.MULTILINE,
        ),
        Entry(
            "bindings/rust/tachyon/Cargo.toml",
            rf'(^\s*version\s*=\s*"){ov}(")',
            rf"\g<1>{new}\g<2>",
            re.MULTILINE,
        ),
        # Java / Kotlin
        Entry(
            "bindings/java/gradle.properties",
            rf"(^version=){ov}",
            rf"\g<1>{new}",
            re.MULTILINE,
        ),
        Entry(
            "bindings/kotlin/gradle.properties",
            rf"(^version=){ov}",
            rf"\g<1>{new}",
            re.MULTILINE,
        ),
        # Node.js
        Entry(
            "bindings/node/package.json",
            rf'("version"\s*:\s*"){ov}(")',
            rf"\g<1>{new}\g<2>",
        ),
        # C#
        Entry(
            "bindings/csharp/src/TachyonIpc/TachyonIpc.csproj",
            rf"(<Version>){ov}(</Version>)",
            rf"\g<1>{new}\g<2>",
        ),
        # README
        Entry(
            "README.md",
            re.escape(old),
            new,
            max_count=0,
        ),
        Entry(
            "bindings/python/README.md",
            re.escape(old),
            new,
            max_count=0,
        ),
        Entry(
            "bindings/rust/tachyon/README.md",
            re.escape(old),
            new,
            max_count=0,
        ),
        Entry(
            "bindings/go/README.md",
            re.escape(old),
            new,
            max_count=0,
        ),
        Entry(
            "bindings/java/README.md",
            re.escape(old),
            new,
            max_count=0,
        ),
        Entry(
            "bindings/kotlin/README.md",
            re.escape(old),
            new,
            max_count=0,
        ),
        Entry(
            "bindings/csharp/README.md",
            re.escape(old),
            new,
            max_count=0,
        ),
    ]


def process(entry: Entry, root: Path, dry_run: bool) -> tuple[bool, str]:
    path = root / entry.rel_path
    if not path.exists():
        return False, f"MISSING   {entry.rel_path}"

    text = path.read_text(encoding="utf-8")
    new_text, count = re.subn(
        entry.pattern,
        entry.replacement,
        text,
        count=entry.max_count,
        flags=entry.flags,
    )
    if count == 0:
        return False, f"NO MATCH  {entry.rel_path}  (pattern: {entry.pattern!r})"

    noun = "replacement" if count == 1 else "replacements"
    msg = f"{'would update' if dry_run else 'updated':12s}  {entry.rel_path}  ({count} {noun})"
    if not dry_run:
        path.write_text(new_text, encoding="utf-8")

    return True, msg


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Bump semver version strings.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("old_tag", help="Previous release tag")
    parser.add_argument("new_tag", help="New release tag")
    parser.add_argument("--dry-run", action="store_true", help="Print what would change without writing any file")
    args = parser.parse_args()

    old = parse_ver(args.old_tag)
    new = parse_ver(args.new_tag)
    if old == new:
        print(f"error: old and new versions are identical ({old})", file=sys.stderr)
        return 1

    root = Path(__file__).resolve().parents[2]
    entries = build_entries(old, new)
    if args.dry_run:
        print(f"Dry run: {old} → {new}\n")

    errors: list[str] = []
    ok_msgs: list[str] = []
    for entry in entries:
        ok, msg = process(entry, root, args.dry_run)
        if ok:
            ok_msgs.append(msg)
        else:
            errors.append(f"  {msg}")

    for msg in ok_msgs:
        print(f"  {msg}")

    if errors:
        print(f"\nerror: {len(errors)} file(s) could not be updated:", file=sys.stderr)
        for e in errors:
            print(e, file=sys.stderr)
        return 1

    action = "Would bump" if args.dry_run else "Bumped"
    print(f"\n{action} {old} -> {new} across {len(ok_msgs)} file(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
