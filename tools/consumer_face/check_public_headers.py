#!/usr/bin/env python3
"""C1 — public headers carry no backend identifiers (spec/architecture/multi-rhi.md §6).

Scans include/pictor/** (except the opt-in interop headers under
include/pictor/backend/) token by token, comments removed, for backend API
identifiers / includes and per-backend macros (face_rules.BACKEND_TOKEN /
BACKEND_INCLUDE).

Headers that still expose a backend are listed in c1_allowlist.txt. The list
only shrinks: the check fails when
  * a header outside the allowlist has a hit (new leak), or
  * an allowlisted header is clean or gone (remove it from the list), or
  * the allowlist size differs from its recorded `# count:` line.

Exit 0 when clean, 1 otherwise. `--list` prints every hit.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from face_rules import REPO_ROOT, backend_hits, iter_cxx_files, rel  # noqa: E402

INCLUDE_ROOT = REPO_ROOT / "include" / "pictor"
INTEROP_DIR = INCLUDE_ROOT / "backend"
DEFAULT_ALLOWLIST = Path(__file__).resolve().parent / "c1_allowlist.txt"
COUNT_LINE = re.compile(r"^#\s*count:\s*(\d+)\s*$")


def load_allowlist(path: Path) -> tuple[list[str], int | None]:
    entries: list[str] = []
    declared: int | None = None
    for line in path.read_text(encoding="utf-8").splitlines():
        m = COUNT_LINE.match(line.strip())
        if m:
            declared = int(m.group(1))
            continue
        line = line.split("#", 1)[0].strip()
        if line:
            entries.append(line)
    return entries, declared


def scan() -> dict[str, list[tuple[int, str]]]:
    result: dict[str, list[tuple[int, str]]] = {}
    for path in iter_cxx_files(INCLUDE_ROOT):
        if INTEROP_DIR in path.parents:
            continue
        hits = backend_hits(path)
        if hits:
            result[rel(path)] = hits
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--allowlist", type=Path, default=DEFAULT_ALLOWLIST)
    ap.add_argument("--list", action="store_true", help="print every hit")
    ap.add_argument("--write-allowlist", action="store_true",
                    help="rewrite the allowlist from the current scan (baseline only)")
    args = ap.parse_args()

    hits = scan()
    scanned = sum(1 for p in iter_cxx_files(INCLUDE_ROOT) if INTEROP_DIR not in p.parents)

    if args.write_allowlist:
        lines = [
            "# C1 allowlist: public headers that still expose a backend (multi-rhi.md §6).",
            "# Shrinks stage by stage; empty at S4. Regenerate only to record a baseline:",
            "#   python3 tools/consumer_face/check_public_headers.py --write-allowlist",
            f"# count: {len(hits)}",
            *sorted(hits),
        ]
        args.allowlist.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"C1: wrote {len(hits)} entries to {rel(args.allowlist)}")
        return 0

    allowed, declared = load_allowlist(args.allowlist)
    allowed_set = set(allowed)
    errors: list[str] = []

    for header in sorted(hits):
        if header not in allowed_set:
            first = ", ".join(f"L{ln} {tok}" for ln, tok in hits[header][:5])
            errors.append(f"new backend leak: {header} ({first})")
    for header in allowed:
        if header not in hits:
            errors.append(f"allowlisted header is clean or gone, remove it: {header}")
    if len(allowed) != len(allowed_set):
        errors.append("allowlist has duplicate entries")
    if declared is None:
        errors.append("allowlist has no '# count: N' line")
    elif declared != len(allowed):
        errors.append(f"allowlist '# count: {declared}' does not match its {len(allowed)} entries")

    print(f"C1: scanned {scanned} public headers; {len(hits)} expose a backend; "
          f"allowlist {len(allowed)} (declared {declared})")
    if args.list:
        for header in sorted(hits):
            print(f"  {header}")
            for ln, tok in hits[header]:
                print(f"    L{ln}: {tok}")
    for e in errors:
        print(f"C1 FAIL: {e}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
