# Kuzuha: research-based curved geometry in Pictor

The default executable now uses the direct polynomial raymarch path described in
[kuzuha-raymarch.md](kuzuha-raymarch.md). This document defines the shared PN
geometry and the optional sampled raster reference.

## SPEC-PC-KUZUHA-PN

`pictor_kuzuha_demo.exe` imports the user's local `ch_Kuzuha_0000.fbx` as a
coarse geometric scaffold and evaluates cubic triangular patches in Pictor's
Vulkan FBX viewer. The input is an intermediate design, not a complete target.
The demonstrated research result is interpolation between existing samples;
it does not claim to infer missing character design or a percentage improvement.

The executable is a Pictor demo, with no dependency on Figmentum/Ergo/Unity.
Geometry, UVs, materials and bone influences come through Pictor's FBX importer.
The old FBX viewer stays available. Optional reconstruction controls are off in
that target; `pictor_kuzuha_demo` enables them and uses executable-relative assets.

## Research and exact scope

Primary method: Alex Vlachos, Jorg Peters, Chas Boyd and Jason L. Mitchell,
**Curved PN Triangles**, ACM Symposium on Interactive 3D Graphics (2001), 159–166.
[Author PDF](https://alex.vlachos.com/graphics/CurvedPNTriangles.pdf),
[DOI 10.1145/364338.364387](https://doi.org/10.1145/364338.364387).

For each triangle with positions P1,P2,P3 and unit normals N1,N2,N3:

```
wij = dot(Pj - Pi, Ni)
b300=P1; b030=P2; b003=P3
b210=(2P1+P2-w12*N1)/3   (and the other five oriented edge coefficients)
E=mean(the six edge coefficients); V=(P1+P2+P3)/3
b111=E+(E-V)/2
S(u,v,w)=sum_(i+j+k=3) [3!/(i!j!k!)] * u^i*v^j*w^k * bijk
u+v+w=1
```

The three corner positions are interpolated. Sampling density controls the
triangular approximation of this fixed continuous polynomial; increasing density
does not author new features. A factor n produces n² triangles per input triangle.
Factor 1 is the source, factor 2 samples edges, factor 4 also samples the interior.

Pictor-specific additions, separate from the original paper:

- Global strength blends the linear triangle and PN patch at the same barycentric
  coordinates: `(1-alpha)*linear + alpha*S`, alpha in 0..1.
- Connectivity uses `(geometry ID, original FBX vertex ID)`, independently of UV
  or material corner splits. Open/nonmanifold edges and inconsistent endpoint
  normals use straight cubic boundary coefficients on both sides. Material
  boundaries alone do not create creases. This conservative policy protects the
  input silhouette of open sheets and hard edges; it is not PN-AEN.
- UV and shading normals are barycentrically interpolated. The original paper
  permits linear normals; its optional quadratic-normal scheme is not used here.
- Bone IDs are merged by identity, the four largest interpolated influences are
  normalized. Default comparison is the bind pose: interpolating weights before
  LBS does not commute with interpolation of already-skinned source positions.
- Source / PN comparisons within each renderer use identical lighting. Clay mode retains imported shading
  normals; facets mode uses the rasterized triangle's geometric normal in the
  raster reference, and analytic PN derivatives in raymarch. Texture
  alpha below 0.35 is discarded, an explicit demo cutout rule, not Unity shader parity.

PN patches generally have positional continuity at matching boundaries, not G1
continuity. Artist-authored shading normals may not describe intended curvature:
strength 0 and source mode remain available for assessing inflation/overshoot.
Directly connected corners from separate geometry objects are not welded.

Relevant follow-up on cracks (background, not claimed as this implementation):
Bryan Dudash and John McDonald, **Crack-Free Point-Normal Triangles using Adjacent
Edge Normals** (NVIDIA, 2010),
[technical whitepaper](https://developer.download.nvidia.com/whitepapers/2010/PN-AEN-Triangles-Whitepaper.pdf).

## Data and lifecycle

`PnModel` owns immutable source samples and flat PN control coefficients.
Raster sampling happens on initialization or explicit level/strength changes, not in the
per-frame hot path. Raymarch evaluates strength on the GPU without resampling.
Geometry buffers are allocated first and replaced after GPU
idle. A bounded output vertex budget rejects excessive refinement.

The shared legacy `PackedMesh` type keeps import-only topology; the legacy binary
cache format stays unchanged. Reconstruction bypasses it because it lacks topology.
No imported asset or generated mesh is committed. Default launch loads packaged
`model/model.fbx`, `model/texture`, and `shaders` beside the exe.

`--capture-set` renders eight images from the actual Vulkan swapchain with controlled
camera/pose/quality. `--pn-report` records renderer, patch count, surface triangle
count, alpha, LOD target, unresolved rays and frame time. PN sampling statistics
describe displacement from the linear sample, not accuracy errors against an
unknown finished character. Results and validation are recorded
in the task report after executing through Excubitor in the project main folder.

## Implementation ownership

- `demo/kuzuha/pn_patch.*`: polynomial coefficients and evaluation.
- `demo/kuzuha/pn_model.*`: imported adjacency, boundary policy, attributed sampling.
- `demo/kuzuha/reconstruction.*`: comparison controls, camera, observation report.
- `demo/fbx_viewer/packed_mesh.h`: shared import/render data contract.
- `demo/fbx_viewer/main.cpp`: reuse existing Vulkan, materials, input and capture integration.
- `tools/package_kuzuha_demo.ps1`: explicit local asset/binary packaging.
