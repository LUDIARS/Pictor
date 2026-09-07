# Polynomial motion SDK and native hair recording

The existing raymarch viewer had static source-derived coefficients. An optional
installed PictorDemo SDK now accepts a caller-owned PolynomialMotion provider,
updates changed coefficients and their bounds after the previous frame fence,
and displays provider controls/status. An upper Fg app consumes this SDK without
adding an upper-library dependency to Pictor. It bends the existing Kuzuha hair
and six fork tips, then supplies the resulting cubic control nets.

The viewer can record actual swapchain images at fixed simulation intervals.
Recording preserves its requested frame sequence even when rendering is slower
than the encoded frame rate; it does not overwrite a populated directory.
Motion mode requires raymarch and disables the raster switch. Interpolation and
numerical LOD controls remain available.

## Validation, 2026-09-08 JST

- Release builds of `pictor_polynomial_demo` and `pictor_kuzuha_demo` succeeded.
  The installed SDK linked successfully into the Fg consumer executable.
- Anatomia domain audit after SDK ownership/spec additions: 69 modules,
  3520 symbols, zero unclassified. Earlier PR #1464 Test OK refers to the prior
  commit; the SDK changes require a new review.
- Pictor main-folder execution through Excubitor after Cc claim produced a
  still and a 300-frame, 1280×720 raymarch video. All seven textures loaded.
  Source surface raster triangle count and previous-frame unresolved-ray
  counts were zero. Updated hair coefficients and AABBs were exercised every
  frame. The positive/reverse/wind-off images were visually inspected.
- The final 10-second H.264/yuv420p MP4 has 300 frames at 30/1 fps and is
  1,336,931 bytes. Lictor accepted delivery to the requesting thread.
- Physics validation belongs to Fg: its dedicated executable passed 10/10
  checks. The final recording's largest frame-sampled stretch was 4.9689%,
  body-proxy residual 0.038706 mm, selected self-contact residual 0.153482 mm.
- No unrelated suites or manual keyboard tests were run. Native Computer Use
  was unavailable. Testing claims were released after capture.

Artifact directory: `Pictor/build/kuzuha-hair-demo/`; final recording:
`pictor-kuzuha-hair-forks-20260908.mp4`; detailed metrics:
`wind-fork-validation.json`. It is a fixed-time recording: interactive rendering
on the GTX 1070 was about 2 fps. This does not claim completion of the separately
requested cel-look renderer, exact garment collision, or a closed-form temporal
hair solution.
