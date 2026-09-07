# Kuzuha polynomial raymarch and live controls

## SPEC-PC-KUZUHA-RAYMARCH

The default Pictor Kuzuha exe renders the PN polynomial directly with bounded
sphere tracing. It displays controls, interpolation alpha, quality, frame time
and unresolved patch-rays inside the Vulkan window. F5 explicitly switches to
the raster reference. There is no automatic fallback from raymarch to raster.

This demonstrates a continuous polynomial surface. It does not establish that
the unfinished FBX is the finished character, or replace it with a compact
semantic character model. FBX positions/normals still initialize 106,386 PN
patches; imported diffuse maps remain image textures. Raymarch uses the bind pose.

## Representation and intersection

Each immutable GPU patch stores ten cubic Bernstein coefficients, three imported
normals, three UVs, a control-hull AABB and curvature bounds. At alpha a:

```
S_a(v,w) = (1-a) L(v,w) + a S_PN(1-v-w,v,w),  0 <= a <= 1
```

The vertex shader draws an instanced screen rectangle covering the padded
control-hull AABB. Its two proxy triangles supply candidate fragments only:
they have no character surface, hit depth, surface UV or shading normal.
The fragment shader reconstructs a unit camera ray, intersects the AABB and
near/far interval, and advances using distance lower bounds to S_a.
Accepted hits evaluate actual polynomial points and material coordinates.
Depth comes from the accepted ray parameter. The analytic normal mode uses
cross(partial S_a/partial v, partial S_a/partial w).

There are zero *character surface* triangle draws in raymarch mode. Screen
proxies and text are still rasterized. The implementation does not bake an
SDF/voxel volume and does not intersect a hidden tessellated character mesh.
An epsilon hit means an evaluated surface point is within epsilon of the ray
point; it is not an exact intersection or a guarantee of epsilon depth error,
especially at grazing incidence.

## Distance lower bound derivation

For the degree-three PN coefficients b_ijk, let i+j+k=1:

```
X_ijk = 6 (b[i,j+2,k] - 2 b[i+1,j+1,k] + b[i+2,j,k])
Y_ijk = 6 (b[i,j,k+2] - 2 b[i+1,j,k+1] + b[i+2,j,k])
Z_ijk = 6 (b[i,j+1,k+1] - b[i+1,j+1,k] - b[i+1,j,k+1] + b[i+2,j,k])
A = max ||X_ijk||; C = max ||Y_ijk||; B = max ||Z_ijk||
H = max(A,C) + B
```

Second derivatives are degree-one Bernstein patches, so their norms are bounded
by their control norms. H bounds the norm of the bilinear Hessian operator.
Four-way midpoint subdivision produces right-isosceles parameter domains with
signed side s. Let T be the linear interpolant of S_a at a domain's corners.
Taylor's remainder and the maximum barycentric variance give:

```
||S_a(v,w) - T(v,w)|| <= delta = a H s^2 / 4
lower(p,domain) = max(distance(p,T) - delta - fpMargin, 0)
upper(p,uv) = ||p - S_a(uv)||
```

For the root domain, delta may also use the minimum with
a*max||b_ijk - (i*P0+j*P1+k*P2)/3||, by the convex-hull property.
H and the root deviation are computed in double and rounded upward to float.

The query subdivides the full parameter triangle, keeps the minimum lower bound
over every unpruned frontier leaf, and prunes only when a domain lower bound is
at least a known actual-point upper bound. Upper bounds are used for hits and
pruning, never as march steps. Degenerate triangle proxies use their edges/points.
A fixed stack, 768 query visits, depth nine and 512 march iterations bound work.
Budget exhaustion or no numerical progress is UNRESOLVED, counted and rendered
magenta; it is never silently classified as empty background.

The lower-bound derivation is conservative in real arithmetic. The shader uses
float evaluation and an engineering roundoff margin; this is not a formal
interval-arithmetic proof for arbitrary world-coordinate magnitudes. Height-
scaled tolerances are calibrated to this local model. The demo reports failures
instead of promising all-view/all-scale convergence.

## Dynamic quality and interpolation

F1/F2/F3/F4 select target tolerances of 1, 0.5, 0.25 and 0.125 pixels.
In auto mode, epsilon follows ray distance and viewport height:
max(8*fpMargin, distance * 2*tan(fovY/2)/height * pixelTolerance).
Thus zoom, view distance and window resizing change numerical detail continuously.
A toggles a fixed world tolerance for comparison. This LOD changes intersection
precision/work; it does not reduce the immutable source patch count or create a
coarser semantic model. Raster reference quality controls sampling factor instead.

Brackets adjust alpha by 0.05 and accept key repeat. S switches alpha 0/1.
I animates alpha smoothly between 0 and 1 on the GPU; the coefficient buffer
stays unchanged. C cycles texture, clay, analytic normals and march-step heatmap.
V and arrows control head/body, orbit and zoom. F5 returns to the raster reference.

The HUD uses Pictor's bitmap text renderer with the demo's compatible render pass.
Coefficient/counter buffers and pipelines belong to RaymarchPass. One frame in
flight, the preceding fence, coherent memory and a shader-to-host barrier protect
counter reads and uniform writes. Allocation occurs at initialization or explicit
raster mode/mesh changes, not on continuously animated raymarch frames.

## Appearance limitations

The shared diffuse cutout threshold is 0.35. Texture LOD zero is explicit because
the iterative per-patch hit path cannot rely on implicit fragment derivatives.
After a transparent hit the same patch is discarded; a folded patch's later
opaque intersection is not searched. Other patches are still evaluated.
Imported normals serve texture/clay lighting; analytic PN normals are a separate
inspection mode. This is not the subsequent cel-lighting/shadow demo.

## Research sources and attribution

- Alex Vlachos, Jorg Peters, Chas Boyd, Jason L. Mitchell. **Curved PN Triangles**,
  I3D 2001, pp.159-166. [Author PDF](https://alex.vlachos.com/graphics/CurvedPNTriangles.pdf),
  [DOI](https://doi.org/10.1145/364338.364387). Source of the cubic PN geometry.
- John C. Hart. **Sphere Tracing: A Geometric Method for the Antialiased Ray
  Tracing of Implicit Surfaces**, The Visual Computer 12, 527-545, 1996.
  [Paper](https://graphics.stanford.edu/courses/cs348b-20-spring-content/uploads/hart.pdf),
  [DOI](https://doi.org/10.1007/s003710050084). Distance-bound marching basis.
- Stefan Roth, Patrick Diezi, Markus Gross. **Ray Tracing Triangular Bezier
  Patches**, 2001. [ETH paper](https://cgl.ethz.ch/Downloads/Publications/Papers/2001/p_Rot01.pdf).
  Related direct-patch ray research; this implementation is not their algorithm.

The Hessian/subdivision bound, PN distance-query combination, proxy pipeline and
interactive tolerance policy are this demo's engineering derivation. The cited
papers are not claimed to contain this particular implementation, to reconstruct
Kuzuha, or to recover the unfinished character design.

## Ownership

- pn_gpu_data: coefficient layout and CPU bounds.
- pn_ray_common/distance/vert/frag: evaluation, query, bounds projection and march.
- raymarch_pass: Vulkan resources, descriptors and synchronization.
- reconstruction: dynamic state and controls.
- debug_hud: on-screen observations and help.
- fbx_viewer/main: integration with existing import, camera and capture lifecycle.
