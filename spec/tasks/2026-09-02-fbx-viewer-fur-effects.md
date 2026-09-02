---
task: fbx-viewer-fur-effects
status: implemented
---

# SPEC-FBX-VIEWER-FUR-EFFECTS — FBX viewer fur/effect test rig

The desktop FBX viewer provides an optional shell-fur pass and supporting
rope-binding, tear, and frame-capture diagnostics for visually evaluating a
locally supplied model. These remain demo-only features and do not add
asset-specific behavior to the Pictor library.

## Required behavior

- Fur, rope deformation/geometry, and tears are disabled or identity
  operations unless explicitly enabled by CLI or keyboard input.
- Rope geometry and deformation reveal the same segment prefix throughout
  wrap animation, including multi-strand patterns.
- Texture-derived eye anchors are selected only from vertices referenced by
  the submesh using that texture.
- Capture requests opt into swapchain transfer-source usage, fail clearly
  when the surface or pixel format is unsupported, and never change usage
  requirements for other `VulkanContext` consumers.
- CPU writes to shared demo GPU resources occur only after the prior frame
  using those resources has completed.
- Private test assets and their internal source locations are not repository
  content; documentation uses only a generic local rig layout.

## Verification

Exercise the viewer with fur, animated multi-strand binding, tears, resize,
and BMP capture while Vulkan synchronization validation is enabled. Confirm
the effects remain attached to the intended material and the captured image
matches the presented frame.
