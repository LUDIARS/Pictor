/**
 * Pictor domain ontology for Anatomia.
 *
 * Pictor is the lowest-layer data-driven Vulkan rendering pipeline (C++20). These
 * DomainDefs teach Anatomia Pictor's internal layering so the supply→verify harness
 * gives Pictor-specific guidance: `where`/`context` land an edit in the right
 * subsystem and surface the applicable rules, and `verify`'s rule_conformance gate
 * flags a diff that calls "up" the frame-pipeline spine, calls back into the
 * renderer facade, or reaches into the C export shim.
 *
 * Rules are derived from spec/subsystem/README.md → "フレームパイプライン順" (the
 * documented per-frame stage order) and CLAUDE.md → "レイヤ依存" / SOLID. Matching is
 * by source PATH (`by: "path"`), since a function's layer is its directory, not its
 * name — C++ bare method names (`initialize`/`record`/`build`) collide across
 * subsystems and would over-match if matched by name. Patterns are tested against
 * forward-slashed paths, so `/scene/` matches src/scene/* and include/pictor/scene/*.
 *
 * core/ is special: it conflates the base value types (core/types.h — the floor,
 * included everywhere) with the top-level composition root (core/pictor_renderer,
 * core/renderer_subsystem_manager — the facade that legitimately wires every
 * subsystem). So the spine pins ONLY `/core/types` at the floor; the facade files
 * are deliberately left out of every tier (a composition root may depend on all),
 * and a separate rule forbids the reverse — subsystems calling back into it.
 *
 * Tuning: if a rule floods with violations it is either real architectural debt
 * worth seeing, or the model is too strict — adjust the tier order / patterns here
 * and re-analyze. This file is the single source of truth for Pictor's
 * machine-checked architecture. See [[project_pictor]] / the KS counterpart
 * spec/anatomia/domains/ks-architecture.mjs for the same pattern.
 */

/** @typedef {{ name: string, description: string, presetRules: Array<{preset: string, params: Record<string, unknown>}>, templateRules: unknown[], cardTemplate?: string }} DomainDef */

/**
 * Frame-pipeline dependency spine, lowest (most depended-upon) → highest. A call
 * from a lower tier up to a higher tier is forbidden (a foundation must not know
 * about the pipeline stage built on top of it). Derived from
 * spec/subsystem/README.md "フレームパイプライン順":
 *   data → scene → update → culling → batch → gpu → [gi → decal → postprocess]
 * with the cross-cutting foundations (memory/surface/shader) + base types pinned
 * below the data stage.
 *
 * SCOPE OF ENFORCEMENT: the spine stops at the render passes. The descriptor /
 * orchestration layers ABOVE the passes — material → visus → pipeline (render pass
 * DAG: registry/compiler/scheduler/profile) — are documented here as the top of the
 * stack but are intentionally NOT in the enforced `layers` array. Reason: every one
 * of those subsystems hand-rolls its own file-local JSON parser helpers
 * (skip_ws / parse_string / consume / expect / fail / parse_number — also duplicated
 * in animation/, see the structuralDup review finding), and Anatomia's current
 * call-edge resolver fans a bare free-function call to OTHER files' same-named
 * definitions instead of preferring the same-file one. That produces phantom
 * "calling up into pipeline/visus/material" edges (98 of them at v1) that are pure
 * resolution artifacts, not real layering debt. Enforcing those tiers would bury any
 * genuine signal under the phantom flood. Re-add material/visus/pipeline to SPINE
 * once Anatomia resolves bare free-function calls with same-file locality.
 *
 * NOT in any tier (intentionally unconstrained by the spine):
 *  - core/ facade (pictor_renderer / renderer_subsystem_manager / gi_facade): the
 *    composition root, allowed to depend on everything (see pictor-composition-root).
 *  - material / visus / pipeline: above the passes, unenforced pending the resolver
 *    fix described above.
 *  - animation / text / vector / ui: feature/widget layers that sit beside the core
 *    pipeline and legitimately reach into several stages.
 *  - profiler: cross-cutting observability, wired in at many points.
 *  - c_api: the outermost boundary (see pictor-c-api-boundary).
 *  - webgl: parallel non-abstract alternative backend to the Vulkan path.
 */
const SPINE = [
  "/core/types",                 // base value types (floor; only this file from core/)
  "/(memory|surface)/",          // bump/pool/GPU allocators + Vulkan/platform context (include out-degree 0)
  "/shader/",                    // SPIR-V → VkPipeline generation (builds on surface)
  "/data/",                      // texture/mesh/model asset lifecycle
  "/scene/",                     // SceneRegistry + ObjectPool SoA store
  "/update/",                    // per-frame transform update scheduler
  "/culling/",                   // WorldPartition → FlatBVH → GPU Hi-Z culling
  "/batch/",                     // BatchBuilder + radix sort → draw batch / indirect
  "/gpu/",                       // GPU-driven pipeline (compute update→cull→LOD→indirect) + buffers
  "/(gi|decal|postprocess)/",    // render passes: shadow/AO/probe, projected decals, post FX (top of enforced spine)
];

/** Every spine/feature subsystem that may NOT call back into the composition root. */
const SUBSYSTEMS_BELOW_FACADE =
  "/(memory|surface|shader|data|scene|update|culling|batch|gpu|gi|decal|postprocess|material|visus|pipeline|animation|text|vector|ui|profiler|webgl)/";

/** @type {DomainDef[]} */
const domains = [
  {
    name: "pictor-pipeline-spine",
    description:
      "Pictor frame-pipeline dependency spine (spec/subsystem/README.md フレームパイプライン順): " +
      "core/types→memory/surface→shader→data→scene→update→culling→batch→gpu→gi/decal/postprocess. " +
      "A lower stage must never call up into a higher one — foundations (types/memory/surface) and " +
      "early pipeline stages must not depend on the stages built on top of them. The descriptor/" +
      "orchestration layers above the passes (material/visus/pipeline), the core/ renderer facade, " +
      "the feature layers (animation/text/vector/ui), profiler, c_api and webgl are intentionally " +
      "outside the enforced spine (see the SPINE comment for why material/visus/pipeline are unenforced).",
    presetRules: [
      {
        preset: "layerDependencyDirection",
        params: { layers: SPINE, by: "path", kind: "calls" },
      },
    ],
    templateRules: [],
    cardTemplate:
      "Summarise where this subsystem sits in the Pictor frame-pipeline spine: which lower stages " +
      "(types/memory/surface, then data→scene→…→gpu→passes) it builds on, and confirm it does not " +
      "call up into a later pipeline stage or the orchestration layer.",
  },
  {
    name: "pictor-composition-root",
    description:
      "Pictor composition root: core/pictor_renderer (PictorRenderer) and " +
      "core/renderer_subsystem_manager (RendererSubsystemManager) are the only place that wires the " +
      "subsystems together — they may depend on everything, but NO subsystem may call back into them. " +
      "Calling the renderer facade from inside a subsystem is an inverted dependency that hides " +
      "coupling and breaks the one-way construction order.",
    presetRules: [
      {
        preset: "forbiddenCall",
        params: {
          callerPattern: SUBSYSTEMS_BELOW_FACADE,
          calleePattern: "/core/(pictor_renderer|renderer_subsystem_manager)",
          by: "path",
          kind: "calls",
        },
      },
    ],
    templateRules: [],
    cardTemplate:
      "Confirm this subsystem is constructed/owned by the renderer facade and never calls back into " +
      "PictorRenderer / RendererSubsystemManager — it should expose its own interface and let the " +
      "composition root drive it.",
  },
  {
    name: "pictor-c-api-boundary",
    description:
      "Pictor C export boundary: src/c_api is the outermost C ABI shim for consumers. Internal " +
      "subsystems must never call inward into c_api — the C++ subsystems are the implementation and " +
      "c_api wraps them, not the reverse. (consumer は抽象 interface に依存、内部 layout を漏らさない.)",
    presetRules: [
      {
        preset: "forbiddenCall",
        params: {
          callerPattern: SUBSYSTEMS_BELOW_FACADE,
          calleePattern: "/c_api/",
          by: "path",
          kind: "calls",
        },
      },
    ],
    templateRules: [],
    cardTemplate:
      "Confirm this code does not call into the c_api export shim — c_api wraps the C++ subsystems for " +
      "the C ABI, the dependency only ever flows c_api → subsystem.",
  },
];

export default domains;
