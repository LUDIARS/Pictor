/// Pictor feature manifest — authored source of truth.
///
/// Each entry describes a CMake option that can be toggled to shape the
/// final `libpictor` artefact. Size numbers are *rough estimates* based
/// on source-line counts and measured static-lib deltas; they give a
/// sense of scale, not exact byte counts. Run
/// `tools/feature-selector/scripts/measure_sizes.py` to refresh them
/// with real measurements for your toolchain.
///
/// Shape of FEATURES.categories[].items[]:
///   id            — the CMake variable exposed by `CMakeLists.txt`
///   label         — human label in the UI
///   default       — the out-of-the-box value (ON / OFF)
///   desc          — one-line description
///   type          — "bool" (default) or "build" (compilation switch)
///   size_delta_kb — estimated size added to libpictor when ON (0 when OFF
///                   is the default). Negative numbers for things that
///                   *reduce* size (e.g., WebGL-only stripping — currently
///                   none). `~` prefix on the string form indicates an
///                   estimate vs. measured.
///   sources       — human-readable list of source files/dirs affected
///   deps          — external dependencies required when ON
///   conflicts     — list of ids that can't be ON simultaneously
///   requires      — ids that must also be ON (logical AND)

export const FEATURES = {
    // libpictor.a baseline (everything off that can be off). Used by the
    // UI's size-estimate arithmetic so "total = base + sum(enabled deltas)".
    base_size_kb: 2600,
    base_notes:
        "Core + memory + surface + pipeline + material + data + text. " +
        "Approximate MSVC /O2 static library size with all optional " +
        "features OFF. Actual number varies by toolchain (10-30% swing " +
        "between MSVC and Clang).",

    categories: [
        {
            id: "rendering",
            label: "Rendering backends",
            desc: "Which GPU / web backends to compile in.",
            items: [
                {
                    id: "PICTOR_BUILD_WEBGL",
                    label: "WebGL2 backend",
                    default: "OFF",
                    desc: "Emscripten-targeted WebGL2 rendering path (src/webgl/). Only " +
                          "useful if you build with emcmake — ignored otherwise.",
                    size_delta_kb: 360,
                    sources: ["src/webgl/* (4 files, ~1000 lines)"],
                    deps: ["Emscripten SDK (via emcmake)"],
                },
                {
                    id: "PICTOR_ENABLE_RIVE",
                    label: "Rive Renderer",
                    default: "OFF",
                    desc: "Link rive-runtime prebuilt + enable GPU path for .riv files. " +
                          "Pulls in harfbuzz, sheenbidi, yoga, and Rive's image backends.",
                    size_delta_kb: 4800,
                    sources: ["src/vector/rive_renderer.cpp"],
                    deps: ["Rive runtime prebuilt (cmake/FindRive.cmake)"],
                },
            ],
        },
        {
            id: "diagnostics",
            label: "Diagnostics & tooling",
            desc: "Profiler and build-time helpers — usually ON in dev, often OFF in shipping.",
            items: [
                {
                    id: "PICTOR_ENABLE_PROFILER",
                    label: "Profiler",
                    default: "ON",
                    desc: "GPU/CPU timers, frame statistics, bitmap text overlay. " +
                          "Defines PICTOR_PROFILER_ENABLED=1 for callers.",
                    size_delta_kb: 230,
                    sources: ["src/profiler/* (7 files, ~1100 lines)"],
                    deps: [],
                },
                {
                    id: "PICTOR_USE_LARGE_PAGES",
                    label: "Large page support",
                    default: "OFF",
                    desc: "Use 2MB pages for Pictor's memory subsystem. Requires OS " +
                          "privilege (SeLockMemoryPrivilege on Windows, transparent " +
                          "huge pages on Linux).",
                    size_delta_kb: 15,
                    sources: ["src/memory/* (guarded by PICTOR_LARGE_PAGES=1)"],
                    deps: ["OS privilege at runtime"],
                },
            ],
        },
        {
            id: "buildsets",
            label: "Build sets",
            desc: "Meta switches that enable extra executables / demos / tools in the build.",
            items: [
                {
                    id: "PICTOR_BUILD_DEMO",
                    label: "Demo executables",
                    default: "ON",
                    desc: "Compile the 12 demo apps under demo/ (benchmark, fbx, " +
                          "mobile, graphics, ocean, text, postprocess, texture2d, " +
                          "rive, rive_cube, fbx_viewer, webgl). Adds binaries " +
                          "next to libpictor but does not inflate libpictor itself.",
                    size_delta_kb: 0,
                    affects_exe_only: true,
                    sources: ["demo/*/main.cpp"],
                    deps: [],
                },
                {
                    id: "PICTOR_BUILD_TOOLS",
                    label: "Developer tools",
                    default: "OFF",
                    desc: "Add custom CMake targets for the tools under tools/ " +
                          "(feature selector, future: spv inspector, etc.). " +
                          "No impact on libpictor size.",
                    size_delta_kb: 0,
                    affects_exe_only: true,
                    sources: ["tools/*"],
                    deps: [],
                },
            ],
        },
    ],
};

// ─── Preset recommendations ────────────────────────────────────────
// A small set of curated bundles for common deployment targets. The UI
// exposes them as "Load preset" buttons that flip the right mix of
// toggles in one click.

export const PRESETS = [
    {
        id: "shipping",
        label: "Shipping (minimum)",
        desc: "Leanest libpictor — no profiler, no demos, no tools. Use this " +
              "for the final game build.",
        settings: {
            PICTOR_BUILD_DEMO:      "OFF",
            PICTOR_ENABLE_PROFILER: "OFF",
            PICTOR_USE_LARGE_PAGES: "OFF",
            PICTOR_BUILD_WEBGL:     "OFF",
            PICTOR_ENABLE_RIVE:     "OFF",
            PICTOR_BUILD_TOOLS:     "OFF",
        },
    },
    {
        id: "shipping_rive",
        label: "Shipping + Rive",
        desc: "Shipping build with Rive vector animation support.",
        settings: {
            PICTOR_BUILD_DEMO:      "OFF",
            PICTOR_ENABLE_PROFILER: "OFF",
            PICTOR_USE_LARGE_PAGES: "OFF",
            PICTOR_BUILD_WEBGL:     "OFF",
            PICTOR_ENABLE_RIVE:     "ON",
            PICTOR_BUILD_TOOLS:     "OFF",
        },
    },
    {
        id: "dev_default",
        label: "Development (default)",
        desc: "Out-of-the-box Pictor dev setup — profiler + demos ON.",
        settings: {
            PICTOR_BUILD_DEMO:      "ON",
            PICTOR_ENABLE_PROFILER: "ON",
            PICTOR_USE_LARGE_PAGES: "OFF",
            PICTOR_BUILD_WEBGL:     "OFF",
            PICTOR_ENABLE_RIVE:     "OFF",
            PICTOR_BUILD_TOOLS:     "OFF",
        },
    },
    {
        id: "webgl",
        label: "Web (WebGL2)",
        desc: "Emscripten-bound build. Use with `emcmake cmake ..`.",
        settings: {
            PICTOR_BUILD_DEMO:      "ON",
            PICTOR_ENABLE_PROFILER: "OFF",
            PICTOR_USE_LARGE_PAGES: "OFF",
            PICTOR_BUILD_WEBGL:     "ON",
            PICTOR_ENABLE_RIVE:     "OFF",
            PICTOR_BUILD_TOOLS:     "OFF",
        },
    },
    {
        id: "everything",
        label: "Kitchen sink",
        desc: "Every feature ON — use to prove the full build still works.",
        settings: {
            PICTOR_BUILD_DEMO:      "ON",
            PICTOR_ENABLE_PROFILER: "ON",
            PICTOR_USE_LARGE_PAGES: "ON",
            PICTOR_BUILD_WEBGL:     "ON",
            PICTOR_ENABLE_RIVE:     "ON",
            PICTOR_BUILD_TOOLS:     "ON",
        },
    },
];
