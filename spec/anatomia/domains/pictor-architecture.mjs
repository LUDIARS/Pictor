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
 * SCOPE OF ENFORCEMENT: the full spine, foundations → passes → descriptor /
 * orchestration (material → visus → pipeline = render pass DAG: registry/compiler/
 * scheduler/profile). The upper three tiers were briefly unenforced at v1 because
 * every serializer hand-rolls a file-local anonymous-namespace `struct Parser`
 * (skip_ws / parse_string / consume / …, also in animation/, see the structuralDup
 * review finding) and Anatomia's resolver fanned those same-named methods across
 * files, producing ~98 phantom "calling up into pipeline/visus/material" edges.
 * Anatomia PR #49 fixed the resolver (same-name file-local types disambiguate by
 * caller locality; an untyped-receiver method call no longer binds to a foreign
 * layer's same-named function), dropping the phantoms 219→3, so the upper tiers are
 * now enforced. The handful of real residual advisories (e.g. surface→pipeline
 * `find_memory_type`, visus→pipeline `pad`) are genuine upward free-calls into
 * helpers that live a layer too high — exactly the coupling this spine should flag.
 *
 * NOT in any tier (intentionally unconstrained by the spine):
 *  - core/ facade (pictor_renderer / renderer_subsystem_manager / gi_facade): the
 *    composition root, allowed to depend on everything (see pictor-composition-root).
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
  "/(gi|decal|postprocess)/",    // render passes: shadow/AO/probe, projected decals, post FX
  "/material/",                  // PBR / custom material builders
  "/visus/",                     // ObjectDescriptor templates (above material)
  "/pipeline/",                  // render pass DAG: registry/compiler/scheduler/profile orchestration (top)
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
      "core/types→memory/surface→shader→data→scene→update→culling→batch→gpu→gi/decal/postprocess→" +
      "material→visus→pipeline. A lower stage must never call up into a higher one — foundations " +
      "(types/memory/surface) and early pipeline stages must not depend on the stages built on top " +
      "of them. The core/ renderer facade, the feature layers (animation/text/vector/ui), profiler, " +
      "c_api and webgl are intentionally outside the spine.",
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
