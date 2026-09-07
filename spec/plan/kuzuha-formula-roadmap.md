# Kuzuha formula model and subsequent cel-rendering demo

Status: follow-up plan; PN interpolation is demonstrated, formula-model completion
and the subsequent cel-rendering demo are pending. User order confirmed 2026-09-07:
finish the Kuzuha formula-modeling plan first, then use it for a Pictor cel-look exe.

The subsequent user request added a bounded-distance PN raymarch renderer,
live tolerance LOD, animated interpolation and in-window controls to the current
demo. See `spec/feature/kuzuha-raymarch.md`. This changes the rendering milestone;
semantic formula-model completion and the cel-lighting demo remain pending.

## What completion must mean

The current PN demo derives one cubic patch per FBX triangle. It preserves an
input mesh dependency and is a local interpolation experiment, not completion of
the character's mathematical representation. Exporting those coefficients alone
would remove a file dependency, but would not give meaningful character controls.

The geometry target is an independently loadable, versioned formula/parameter
model, with semantic controls for face, hair and clothing. The FBX supplies initial
measurements and connectivity; the user's "50%" assessment is a statement about
design completeness, not a numerical error bound or a uniform fitting weight.
Reference silhouettes, landmarks and deliberate edits must guide refinements.
Completion needs visual review of character identity, not merely low FBX error.

Geometry, animation and appearance are separate contracts. Procedural geometry
does not automatically make textures procedural. Any retained image-based material
input must be stated explicitly; it must not be advertised as an all-procedural
character. Likewise, formula evaluation still uses finite numerical precision.

## Feasibility and sequence

1. Establish a head-focused model with explicit regions, boundaries and editable
   shape coefficients. Record which features are constrained by FBX, references or
   a design decision. Extend the proven representation to hair, clothing and detail.
2. Evaluate geometry from that model without loading FBX or pre-tessellated character
   triangles at runtime. Preserve material coordinates and stable part identifiers.
3. Add screen-error-driven sampling and a coarser model hierarchy. Validate camera
   transitions, shared boundaries and silhouette stability on the actual Pictor exe.
4. Assess direct ray intersection of parametric patches versus distance-field sphere
   tracing on the same accepted character regions. Measure image error, missed thin
   features, ray steps and frame time; no performance promise precedes measurement.
5. After the formula-model milestone is accepted, build the cel-look Pictor exe with
   movable lighting, geometric shadows, artistic controls and captured comparisons.

Candidate representations should follow each part's geometry: smooth surface
patches or fitted implicit functions for head/body, curve-guided swept sections
for hair, thin parametric sheets for clothes, and explicit sharp boundaries for
ornament. Combining all parts with smooth union would erase deliberate separation.
This is an engineering proposal, not a claim that a cited method already reproduces
Kuzuha or supplies the missing design automatically.

## Dynamic LOD

PN's near-view sampling can move to GPU tessellation. Shared edges need the same
factor and boundary evaluation; screen-space error and a transition policy matter
more than a single distance threshold. Pictor already enables supported Vulkan
tessellation and has an ocean tessellation demo, but the Kuzuha path is not wired.

The current 106,386 patches impose a floor of 106,386 triangles at factor 1.
Getting below that requires a coarser representation/patch hierarchy, not just
lower tessellation factors. Raymarching has analogous LOD decisions for detail
evaluation, tolerances and spatial bounds; it does not eliminate LOD design.

Research basis: Boubekeur and Schlick, **Generic Adaptive Mesh Refinement**,
[GPU Gems 3, Chapter 5](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-5-generic-adaptive-mesh-refinement).
It describes adaptive barycentric refinement, including PN-based smoothing.

## Direct rays and raymarching

A PN patch is a parametric surface S(u,v). One may intersect rays with that surface
using bounded subdivision/root finding and spatial acceleration, without first
converting the entire character into a signed distance field. The current demo
now uses bounded-distance sphere tracing of PN patches. Direct polynomial
root-intersection remains an alternative for later performance comparisons.

Relevant direct-intersection research: Roth, Diezi and Gross, **Ray Tracing
Triangular Bézier Patches** (2001),
[ETH paper](https://cgl.ethz.ch/Downloads/Publications/Papers/2001/p_Rot01.pdf).

For sphere tracing, an implicit function F(x)=0 is not automatically a distance
function. A safe distance bound (for example, derived from a Lipschitz bound) is
needed to avoid stepping through surfaces. Deformation changes those bounds.
Thin sheets, grazing rays, boundary joins and material coordinates need explicit
handling. Evaluating every character part on every ray step is not a viable default.
Pictor's `sharc_march.comp` currently traverses a spatial hash grid for SHaRC cell
requests; it is not a ready-made Kuzuha SDF surface renderer.

Research basis: Hart, **Sphere Tracing: A Geometric Method for the Antialiased Ray
Tracing of Implicit Surfaces**, The Visual Computer 12, 527–545 (1996),
[paper](https://graphics.stanford.edu/courses/cs348b-20-spring-content/uploads/hart.pdf),
[DOI](https://doi.org/10.1007/s003710050084).
For position/normal-driven implicit fitting, Macêdo, Gois and Velho,
**Hermite Radial Basis Functions Implicits**, Computer Graphics Forum 30(1),
27–42 (2011), [publisher](https://doi.org/10.1111/j.1467-8659.2010.01785.x),
supports interpolation/approximation of oriented samples; it does not by itself
guarantee a true SDF, semantic editability or cheap evaluation of a whole character.

## Cel lighting after the shape milestone

Rays can query the same shape from the camera, lights and secondary directions.
This enables geometrically consistent visibility and self-shadowing beyond the
camera's visible surfaces. Smooth formula derivatives can supply geometric normals
where the surface is differentiable. These are useful ingredients for cel rendering.

Raymarching supplies intersections. Soft shadows still require emitter sampling or
an explicitly approximate shadow model; reflections and indirect illumination
require additional light-transport work. A complete light-transport solver remains
finite-sample/finite-precision, and does not by itself produce the desired anime look.

The planned cel demo should separately expose shading-band thresholds, shadow
color, hair-to-face shadow policy, face-specific normal/threshold edits, highlights,
rim light and outlines. Geometry and shading normals may intentionally differ.
Examples such as suppressing an unwanted nose shadow require artistic direction
even if the geometric intersection is accurate.

Research basis: Todo, Anjyo, Baxter and Igarashi, **Locally Controllable Stylized
Shading**, ACM TOG 26(3), Article 17 (2007),
[author-hosted paper](https://www-ui.is.s.u-tokyo.ac.jp/~takeo/papers/todo_siggraph2007_shading.pdf).
Its local shade/light edits complement global lighting; it is not an SDF renderer.
