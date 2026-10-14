#!/usr/bin/env python3
"""C2 (macro half) — which backend macros reach consumers (spec/architecture/multi-rhi.md §6).

Reads the file CMake generates at configure time
(<build>/consumer_face/interface_definitions.txt, one `target=def;def;...`
line per target, values of $<TARGET_PROPERTY:t,INTERFACE_COMPILE_DEFINITIONS>
including transitive ones) and reports every backend macro a consumer of the
target would see.

Rules:
  * pictor_core must export no backend macro at all.
  * pictor may export only the macros in c2_macro_allowlist.txt (the ones that
    exist before S4 makes them PRIVATE). A new one fails the check.
  * both must export PICTOR_HAS_RHI.

Exit 0 when the rules hold, 1 otherwise. The report is printed either way.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_ALLOWLIST = HERE / "c2_macro_allowlist.txt"

BACKEND_MACRO = re.compile(
    r"^(PICTOR_RHI_\w+|PICTOR_HAS_(?:VULKAN|DX12|D3D12|METAL|WEBGL|WEBGL2|WEBGPU)|VK_\w+|WGPU_\w+)$"
)
NEUTRAL_RHI_MACRO = "PICTOR_HAS_RHI"


def macro_name(definition: str) -> str:
    return definition.split("=", 1)[0].strip()


def parse_definitions(path: Path) -> dict[str, list[str]]:
    targets: dict[str, list[str]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            continue
        target, defs = line.split("=", 1)
        targets[target.strip()] = [macro_name(d) for d in defs.split(";") if d.strip()]
    return targets


def load_allowlist(path: Path) -> set[str]:
    names = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.add(line)
    return names


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--defs", type=Path, required=True,
                    help="<build>/consumer_face/interface_definitions.txt")
    ap.add_argument("--allowlist", type=Path, default=DEFAULT_ALLOWLIST)
    args = ap.parse_args()

    targets = parse_definitions(args.defs)
    allowed = load_allowlist(args.allowlist)
    errors: list[str] = []

    for target in ("pictor_core", "pictor"):
        if target not in targets:
            errors.append(f"{target}: not present in {args.defs}")
            continue
        names = targets[target]
        backend = sorted({n for n in names if BACKEND_MACRO.match(n)})
        print(f"C2: {target} exports {len(names)} definitions; backend macros: "
              f"{', '.join(backend) if backend else '(none)'}")
        if NEUTRAL_RHI_MACRO not in names:
            errors.append(f"{target}: {NEUTRAL_RHI_MACRO} is not exported")
        if target == "pictor_core":
            for n in backend:
                errors.append(f"pictor_core: backend macro must stay PRIVATE: {n}")
        else:
            for n in backend:
                if n not in allowed:
                    errors.append(f"pictor: new backend macro reaches consumers: {n} "
                                  f"(make it PRIVATE; consumers only get {NEUTRAL_RHI_MACRO})")

    for e in errors:
        print(f"C2 FAIL: {e}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
