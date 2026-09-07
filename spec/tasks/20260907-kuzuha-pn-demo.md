# Kuzuha PN interpolation demonstration, 2026-09-07

## Current delivery: polynomial raymarch, live controls and HUD

The follow-up request replaces the default character surface draw with bounded-
distance sphere tracing of the continuous PN polynomial. Controls are printed in
the Vulkan viewport. F1-F4 change numerical LOD; A switches distance/viewport-
dependent and fixed tolerance; brackets/S/I change or animate interpolation;
F5 selects the explicit raster reference. An epsilon value at the camera target
makes camera-dependent LOD observable. This remains FBX-derived geometry, not
completion of the semantic formula-character roadmap.

Release x64, NVIDIA GeForce GTX 1070, 1280x720, launched only through Excubitor
from the staged Pictor main-folder exe after Concordia claims:

| Captured case | Alpha | Target pixel tolerance | Unresolved patch-rays | Frame ms |
|---|---:|---:|---:|---:|
| Linear head | 0 | 0.25 | 0 | 15.27 |
| PN head | 1 | 0.25 | 0 | 254.39 |
| Linear geometric normals | 0 | 0.25 | 0 | 27.06 |
| PN analytic normals | 1 | 0.25 | 0 | 238.47 |
| PN whole body | 1 | 0.25 | 0 | 82.70 |
| Half interpolation | 0.5 | 0.25 | 0 | 157.47 |
| Lower numerical LOD | 1 | 1 | 0 | 171.97 |
| March-step heatmap | 1 | 0.25 | 0 | 269.37 |

Every case evaluates 106,386 PN patches with **zero character surface triangle
draws**. AABB proxy rectangles and debug text still use rasterization. The images
are actual swapchain captures under `raymarch-results/`, with no image retouching.
Counters are from the preceding frame and include occluded patch-rays.
Timing is an exponential average of application frame intervals after 32 frames
per case, including GPU/CPU wait and presentation. It is not a GPU microbenchmark.
The initial eight-frame timings were biased toward the preceding case; the table
above supersedes them. High-quality head rendering is about 4 FPS on this GPU.
Performance work, other GPUs and arbitrary-angle convergence remain unverified.

The animated exe was also started through Excubitor using
`--animate-interpolation --capture .../animated.bmp --capture-frame 90 --stay`.
Its actual capture reported alpha 0.282632, 54.03 ms and zero preceding-frame
unresolved patch-rays. The app stays open with interpolation running. The GUI
automation helper failed to connect to its native pipe, including after a session
reset, so injected/manual key presses and a live resize were **not** exercised.
The automatic capture did exercise quality changes, alpha changes and head/body
camera changes. No unit/integration suites were run.

Debug and Release builds succeeded. Anatomia program-domain analysis found
69 modules / 3,506 symbols, with zero unclassified modules or symbols.
Astra reviewed the polynomial Hessian bounds and full-domain minimum/pruning.
The near/far search interval was corrected before the final capture. Floating-
point conservatism and subsequent opaque hits after a same-patch cutout remain
explicit limitations in `spec/feature/kuzuha-raymarch.md`.

The first 640x360 trial exposed 157 unresolved patch-rays at alpha 1. Raising
bounded search budgets removed those failures in the recorded 1280x720 cases.
A path-encoded traversal and local nearest-point iteration were explored but
increased observed frame times, so the final implementation retains the bounded
stack traversal. The PN shaders compile with SPIR-V optimization enabled.
The final capture added no stderr output; the capture log retains one old,
previously fixed cache message from the original demo run.

One Excubitor restart request was rejected by automatic approval review because
it could not confirm the claim for that restart. The existing Cc claim was read
back, updated with the animation-capture purpose, and the subsequent claim-gated
Excubitor restart succeeded. No alternate launch path was used.

The TestWorkflow forum remains the requested review-record destination. Direct
forum posting was unavailable through this session's destination-bound relay;
the detailed validation is therefore included here for Revisor and the actual
images/package are shared through the current session relay.

## Previous milestone: sampled PN raster reference

The following observations describe the earlier raster implementation, retained
as the explicit F5 comparison path. They do not describe the default raymarch draw.

## Delivered scope

Pictor evaluates cubic PN patches derived from the local kuzuha FBX's positions
and normals, then rasterizes samples in Vulkan. This is a research-based curved
interpolation demo. It is **not** a compact semantic formula model of the whole
character, and it still needs the FBX as input. It does not infer the unfinished
design. The existing FBX is a scaffold, not a complete ground-truth target.

The method and source equations are in `spec/feature/kuzuha-reconstruction.md`;
launch instructions are in `spec/setup/kuzuha-reconstruction.md`.

## Observed results

Release x64 build, NVIDIA GeForce GTX 1070, 1280x720 Vulkan swapchain captures.
The app was staged under the Pictor main project folder and started through
Excubitor (`pc-kuzuha-capture`) after a Concordia testing claim. It generated all
five images and exited; Excubitor reported stopped. The claim was released.

| Measurement | Observed value |
|---|---:|
| Original triangles | 106,386 |
| PN factor | 4 |
| Output triangles | 1,702,176 |
| Protected open/nonmanifold/normal-discontinuous edges | 38,469 |
| Maximum sampled displacement from the linear triangle | 1.6189345 FBX units |
| RMS sampled displacement from the linear triangle | 0.027980255 FBX units |
| Diffuse textures loaded / missing | 7 / 0 |

Displacement is a change from the source, not reconstruction error against an
unknown finished model. The source reports centimeters. Protected-edge count
does not certify watertightness across separate FBX geometry objects.

Images: `source-head`, `pn-head`, `source-facets`, `pn-facets`, `pn-body`.
The textured comparisons retain recognizable appearance; geometric-normal
comparisons expose the additional curvature on cheeks, hair and shoulders.
Open sheets, sharp details and disconnected parts retain source limitations.
PNG files are lossless encodings of the actual BMP captures, without retouching.

Artifacts are local under `Pictor/build/kuzuha-demo/`: executable, FBX, diffuse
maps, SPIR-V shaders, research/setup documents and `results/`. Source assets and
generated artifacts are not committed. The report JSONL appends runs; the initial
source-only entry came from the interrupted Debug attempt described below.

## Validation and limits

- Debug and Release executable builds succeeded; no unit/integration suites ran.
- Astra's read-only review found no blocking error in PN coefficients, shared-edge
  orientation, tessellation indices or the stated research scope.
- Anatomia program-domain analysis reported zero unclassified modules/symbols.
- The completed Release run exercised source and factor-4 mesh replacement,
  texture/facet display and head/body cameras. Manual keys, factors 2/8, optional
  skinning and other GPUs were not separately exercised.
- First Debug capture was stopped through Excubitor while the legacy tear-effect
  eye analysis repeatedly decoded textures. Reconstruction now skips that analysis
  and reuses loaded textures; Release loaded seven maps with no missing textures.
- A reconstruction cache-bypass condition initially emitted a false cache-write
  error. It was corrected; the completed Release run added no stderr output.

This milestone demonstrates continuous local interpolation. A character model
with a small set of semantic shape parameters and editable design residuals is
not implemented by this change.

The subsequent order confirmed by the user is formula-model completion followed
by a cel-look Pictor demo. Feasibility, research references and pending milestones
are tracked in `spec/plan/kuzuha-formula-roadmap.md`.
