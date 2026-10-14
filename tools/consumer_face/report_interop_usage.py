#!/usr/bin/env python3
"""C4 — how much of demo/ tools/ tests/ still reaches past the neutral face
(spec/architecture/multi-rhi.md §6).

Counts, per directory:
  interop_includes     #include of an interop header (pictor/backend/*_interop.h,
                       pictor/pictor_vulkan.h)
  backend_files        source files that name a backend API identifier / header
                       (same rules as C1)

The numbers may only go down. c4_baseline.json holds the last recorded counts;
an increase fails the check (give the reason in the PR and re-record with
--write-baseline). `--markdown` prints a table for the PR body / job summary.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from face_rules import (  # noqa: E402
    INTEROP_HEADER_INCLUDE, REPO_ROOT, backend_hits, iter_cxx_files, read_source, strip_comments,
)

SCOPES = ("demo", "tools", "tests")
METRICS = ("interop_includes", "backend_files")
DEFAULT_BASELINE = Path(__file__).resolve().parent / "c4_baseline.json"


def measure() -> dict[str, dict[str, int]]:
    counts: dict[str, dict[str, int]] = {}
    for scope in SCOPES:
        interop = 0
        backend_files = 0
        root = REPO_ROOT / scope
        if root.is_dir():
            for path in iter_cxx_files(root):
                text = strip_comments(read_source(path))
                interop += len(INTEROP_HEADER_INCLUDE.findall(text))
                if backend_hits(path):
                    backend_files += 1
        counts[scope] = {"interop_includes": interop, "backend_files": backend_files}
    return counts


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    ap.add_argument("--markdown", action="store_true")
    ap.add_argument("--write-baseline", action="store_true")
    args = ap.parse_args()

    counts = measure()
    if args.write_baseline:
        args.baseline.write_text(json.dumps(counts, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"C4: wrote baseline {args.baseline.name}")
        return 0

    baseline = json.loads(args.baseline.read_text(encoding="utf-8")) if args.baseline.exists() else {}
    errors: list[str] = []
    rows: list[tuple[str, str, int, int | None]] = []
    for scope in SCOPES:
        for metric in METRICS:
            now = counts[scope][metric]
            before = baseline.get(scope, {}).get(metric)
            rows.append((scope, metric, now, before))
            if before is not None and now > before:
                errors.append(f"{scope}/{metric} increased {before} -> {now}")

    if args.markdown:
        print("| scope | metric | now | baseline |")
        print("|---|---|---:|---:|")
        for scope, metric, now, before in rows:
            print(f"| {scope} | {metric} | {now} | {'-' if before is None else before} |")
    else:
        for scope, metric, now, before in rows:
            print(f"C4: {scope}/{metric} = {now} (baseline {'-' if before is None else before})")
    for e in errors:
        print(f"C4 FAIL: {e}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
