"""Shared rules for the consumer-face checks (spec/architecture/multi-rhi.md §6 C1 / C2 / C4).

"Backend-specific" means a rendering backend's own types, functions, macros or
headers (Vulkan, D3D12 / DXGI, Metal, WebGPU, WebGL / GLES, Emscripten).
Pictor's own backend-neutral names (BackendKind::Vulkan, "vulkan" labels) are
not backend-specific: the checks look for the backend APIs' identifiers.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Identifiers that belong to a backend API, or to Pictor's per-backend macros.
BACKEND_TOKEN = re.compile(
    r"\b("
    r"Vk[A-Z]\w*|vk[A-Z]\w*|VK_[A-Z0-9_]+"           # Vulkan
    r"|ID3D12\w*|IDXGI\w*|D3D12_\w*|DXGI_\w*"        # Direct3D 12 / DXGI
    r"|MTL[A-Z]\w*|CAMetal\w*"                       # Metal
    r"|WGPU\w*|wgpu[A-Z]\w*"                         # WebGPU
    r"|GL(?:uint|int|enum|sizei|float|boolean|char|bitfield)\b"  # GL / WebGL
    r"|EMSCRIPTEN_\w*"                               # Emscripten
    r"|PICTOR_RHI_[A-Z0-9_]+"                        # backend build switches
    r"|PICTOR_HAS_(?:VULKAN|DX12|D3D12|METAL|WEBGL|WEBGL2|WEBGPU)"
    r")"
)

# Backend API headers.
BACKEND_INCLUDE = re.compile(
    r'^[ \t]*#[ \t]*include[ \t]*[<"]'
    r"(vulkan/|d3d12|dxgi|d3dcompiler|Metal/|QuartzCore/|webgpu/|dawn/|GLES2/|GLES3/|emscripten[/.])",
    re.MULTILINE,
)

# Interop headers: the opt-in, backend-specific face kept for the migration
# period (include/pictor/backend/<backend>_interop.h) and the Vulkan-path
# umbrella that replaced the Vulkan half of pictor.h.
INTEROP_HEADER_INCLUDE = re.compile(
    r'^[ \t]*#[ \t]*include[ \t]*[<"]pictor/(backend/\w+_interop\.h|pictor_vulkan\.h)[>"]',
    re.MULTILINE,
)

CXX_SUFFIXES = {".h", ".hpp", ".hh", ".inl", ".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}


def strip_comments(source: str) -> str:
    """Remove // and /* */ comments, keeping string / char literals and line
    breaks (so reported line numbers stay correct)."""
    out: list[str] = []
    i, n = 0, len(source)
    while i < n:
        c = source[i]
        nxt = source[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = source.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and nxt == "*":
            j = source.find("*/", i + 2)
            block = source[i : (n if j < 0 else j + 2)]
            out.append("\n" * block.count("\n"))
            i = n if j < 0 else j + 2
        elif c in "\"'":
            j = i + 1
            while j < n and source[j] != c and source[j] != "\n":
                j += 2 if source[j] == "\\" else 1
            out.append(source[i : j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def read_source(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig", errors="replace")


def backend_hits(path: Path) -> list[tuple[int, str]]:
    """(line, token) for each backend identifier / include in a source file."""
    text = strip_comments(read_source(path))
    hits: list[tuple[int, str]] = []
    for m in BACKEND_INCLUDE.finditer(text):
        hits.append((text.count("\n", 0, m.start()) + 1, "#include " + m.group(1)))
    for m in BACKEND_TOKEN.finditer(text):
        hits.append((text.count("\n", 0, m.start()) + 1, m.group(1)))
    hits.sort()
    return hits


def iter_cxx_files(root: Path):
    for p in sorted(root.rglob("*")):
        if p.is_file() and p.suffix in CXX_SUFFIXES:
            yield p


def rel(path: Path) -> str:
    return path.resolve().relative_to(REPO_ROOT).as_posix()
