#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Measure real libpictor sizes per feature combination.

Build `libpictor` once with every feature OFF (baseline), then once per
feature with that feature flipped to ON, and record the byte delta into
a JSON report that the static HTML tool can consume.

Usage:
    python3 tools/feature-selector/scripts/measure_sizes.py \\
        --build-dir /tmp/pictor-measure \\
        --config Release \\
        --out tools/feature-selector/measurements.json

Notes:
- The script only toggles ONE feature at a time against the baseline.
  That's a linear additive model — fine for features that don't share
  code paths, but it ignores cross-feature synergy (e.g., Rive + WebGL
  both pulling in colour pipelines). Accept the simplification for MVP.
- Requires CMake + a working toolchain. Does NOT require that each
  feature's external dependency is present — skips and records "n/a"
  if `cmake` fails to configure the variant.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
from pathlib import Path

ALL_FEATURES = [
    # (cmake_id, default_value, affects_exe_only?)
    ("PICTOR_BUILD_DEMO",       "ON",  True),
    ("PICTOR_ENABLE_PROFILER",  "ON",  False),
    ("PICTOR_USE_LARGE_PAGES",  "OFF", False),
    ("PICTOR_BUILD_WEBGL",      "OFF", False),
    ("PICTOR_ENABLE_RIVE",      "OFF", False),
    ("PICTOR_BUILD_TOOLS",      "OFF", True),
]


def candidate_lib_paths(build_dir: Path, config: str) -> list[Path]:
    """Where the Pictor static lib typically ends up per toolchain."""
    names = ["libpictor.a", "pictor.lib", "libpictor.so", "pictor.dll"]
    roots = [
        build_dir,
        build_dir / config,        # MSBuild multi-config
        build_dir / "lib",
        build_dir / "lib" / config,
        build_dir / "src",
        build_dir / "src" / config,
    ]
    out: list[Path] = []
    for r in roots:
        for n in names:
            out.append(r / n)
    return out


def find_lib(build_dir: Path, config: str) -> Path | None:
    for p in candidate_lib_paths(build_dir, config):
        if p.is_file():
            return p
    # last-ditch scan.
    for p in build_dir.rglob("*pictor*"):
        if p.is_file() and p.suffix in (".a", ".lib", ".so", ".dll"):
            return p
    return None


def run(cmd: list[str], *, cwd: Path | None = None, check: bool = True) -> tuple[int, str]:
    print("  $ " + " ".join(str(c) for c in cmd))
    proc = subprocess.run(
        cmd, cwd=cwd, capture_output=True, text=True, errors="replace",
    )
    if check and proc.returncode != 0:
        sys.stderr.write(proc.stdout + "\n" + proc.stderr + "\n")
    return proc.returncode, proc.stdout + proc.stderr


def configure_and_build(src_dir: Path, build_dir: Path, config: str,
                        settings: dict[str, str]) -> tuple[bool, str]:
    if build_dir.exists():
        # Fresh cache so -D changes take effect cleanly.
        for p in list(build_dir.glob("CMakeCache.txt")) + list(build_dir.glob("CMakeFiles")):
            # Best-effort delete; let CMake regenerate everything.
            try:
                if p.is_dir():
                    import shutil
                    shutil.rmtree(p)
                else:
                    p.unlink()
            except Exception:
                pass
    build_dir.mkdir(parents=True, exist_ok=True)

    args = ["cmake", "-S", str(src_dir), "-B", str(build_dir)]
    for k, v in settings.items():
        args.append(f"-D{k}={v}")
    rc, log = run(args, check=False)
    if rc != 0:
        return False, log

    rc, log = run(["cmake", "--build", str(build_dir), "--config", config,
                   "--target", "pictor"], check=False)
    return rc == 0, log


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src-dir",   default=str(Path(__file__).resolve().parents[3]),
                    help="Pictor source root (default: auto-detected from this script)")
    ap.add_argument("--build-dir", required=True, help="Scratch build directory")
    ap.add_argument("--config",    default="Release", help="CMake --config value")
    ap.add_argument("--out",       required=True, help="Output JSON path")
    ap.add_argument("--skip",      action="append", default=[],
                    help="Feature ids to skip (e.g. PICTOR_BUILD_WEBGL needs emcmake)")
    opts = ap.parse_args(argv)

    src_dir   = Path(opts.src_dir).resolve()
    build_dir = Path(opts.build_dir).resolve()
    out_path  = Path(opts.out).resolve()

    print(f"=== Pictor feature-size measurement ===")
    print(f"  src = {src_dir}")
    print(f"  build = {build_dir}")

    # Baseline: everything at its "minimum" value (for boolean options we
    # pick OFF regardless of default — the point is to measure delta).
    baseline_settings = {k: "OFF" for (k, _def, _exe) in ALL_FEATURES}
    print("\n[baseline] building with every option OFF …")
    ok, log = configure_and_build(src_dir, build_dir, opts.config, baseline_settings)
    if not ok:
        print("Baseline build failed. Log:\n" + log)
        return 2
    base_lib = find_lib(build_dir, opts.config)
    if base_lib is None:
        print("Baseline build succeeded but no pictor library found.")
        return 3
    base_size = base_lib.stat().st_size
    print(f"  baseline libpictor = {base_lib} ({base_size:,} bytes)")

    results = []
    for (fid, _def, exe_only) in ALL_FEATURES:
        if fid in opts.skip:
            print(f"[{fid}] skipped by --skip")
            results.append({"id": fid, "skipped": True, "reason": "--skip"})
            continue
        if exe_only:
            print(f"[{fid}] skipped (affects executables only, not libpictor)")
            results.append({"id": fid, "skipped": True, "reason": "affects_exe_only"})
            continue

        settings = dict(baseline_settings)
        settings[fid] = "ON"
        print(f"\n[{fid}] building with ON …")
        ok, log = configure_and_build(src_dir, build_dir, opts.config, settings)
        if not ok:
            print(f"  build failed — recording as unavailable. Tail of log:")
            print("  | " + "\n  | ".join(log.splitlines()[-10:]))
            results.append({"id": fid, "skipped": True, "reason": "build_failed"})
            continue
        lib = find_lib(build_dir, opts.config)
        if lib is None:
            results.append({"id": fid, "skipped": True, "reason": "lib_missing"})
            continue
        size = lib.stat().st_size
        delta_kb = round((size - base_size) / 1024, 1)
        print(f"  {lib.name} = {size:,} bytes (delta {delta_kb:+.1f} KB)")
        results.append({
            "id":           fid,
            "size_bytes":   size,
            "delta_bytes":  size - base_size,
            "delta_kb":     delta_kb,
        })

    report = {
        "schema_version": 1,
        "measured_at":    dt.datetime.utcnow().isoformat(timespec="seconds") + "Z",
        "toolchain":      {
            "platform": sys.platform,
            "config":   opts.config,
        },
        "base_bytes":     base_size,
        "base_kb":        round(base_size / 1024, 1),
        "features":       results,
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"\nWrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
