# SPEC-PC-POLYNOMIAL-MOTION — External polynomial motion and frame recording

The native research viewer accepts a borrowed `PolynomialMotion` implementation
through an optional `PictorDemo` SDK. The caller owns the provider for the whole
blocking viewer invocation. Pictor owns the window, Vulkan resources, PN
distance bounds, sparse coefficient uploads and captures. Pictor has no link
or source dependency on the caller's physics library.

Initialization supplies temporary read-only views of source vertices, skin
weights, world bind-bone positions, triangle indices and cubic Bernstein patches.
The provider allocates fixed patch storage and a list of changed indices once.
It may append at most 65,536 opaque constant-color polynomial patches. Texture
coordinates and smooth normals remain attached to the source patches.

Each frame updates the provider and only changed GPU patches after the previous
frame fence. The persistently mapped coherent buffer is released by RaymarchPass.
Every changed polynomial gets a fresh control-hull AABB, deviation and Hessian
bound. Raymarch geometry and bounds therefore follow the same deformation.
No physical state, units, hair semantics or wind solver is implemented in Pictor.

Provider mode uses raymarch; F5 raster switching is disabled and its HUD label
changes accordingly. The existing polynomial interpolation and numerical LOD
controls remain available. The provider receives 0/1/2/P/R commands for zero,
positive, negative external force, pause and reset, and supplies its own status.
These controls are distinct from viewer animation/bind-pose controls.

`--record-frames <new-or-empty-directory> --record-count 300 --record-fps 30`
captures consecutive actual swapchain images as `frame-00000.bmp` etc.
Reconstruction/provider time advances by exactly 1/fps per rendered frame,
independent of wall rendering time. This is an offline fixed-time render, not
a claim of achieving the encoded video frame rate interactively. Simulation
providers still own their smaller numerical substeps. Recording cannot be
combined with `--capture` or `--capture-set`. Existing files are not overwritten.

The installed SDK is opt-in (`PICTOR_BUILD_POLYNOMIAL_SDK=ON`), currently Windows
desktop without Rive/PCM. It exports Pictor, native demo support and the fetched
GLFW dependency, or finds an installed GLFW when configured that way. Consumers
use `find_package(PictorDemo CONFIG REQUIRED)`; sibling source dependencies are
unnecessary. Pictor's upper-library dependency rule remains intact.

Validation for the Fg consumer: build the SDK, build the upper demo, stage its
exe/model/shaders under Pictor's main folder, claim testing in Cc and launch via
Excubitor. The dedicated video service records real raymarched hair deformation.
This extends SPEC-PC-KUZUHA-RAYMARCH; its numerical/error limitations still apply.
