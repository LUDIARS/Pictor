# Kuzuha stage C production package

Build the existing Astra PN reconstruction demo with `pictor_kuzuha_demo` in
Release, with tests disabled. Stage the executable and SPIR-V shaders in
`Pictor/build/kuzuha-stage-demo/`. The generated stage services in the owner
catalog deliberately use this main-project package, never a worktree launch.

Copy the original demo's `model/model.fbx` and seven texture images into the new
package. Replace only this copy's Face and Crown maps with the generated
candidate assets after checking their UV placement on the model. Keep originals
and generated outputs side by side outside git; do not overwrite user originals.

Supply `ks-turn-reference.txt`: increasing whitespace-separated `time value`
keys extracted from KS `Kuzuha_turnR_ver1.fbx` / `jnt_Head`, normalized by the
largest local rotation span. The production sample uses the existing extraction
from the Fg closeup pipeline. This drives camera orbit over 18 seconds; it is not
skeletal retargeting or playback of the full KS motion.

The `pc-kuzuha-stage-still` service writes an 800x960 BMP. The
`pc-kuzuha-stage-video` service writes 432 BMPs at fixed simulated 24 fps.
Concordia testing claim/release and Excubitor control are required for recording.
`stage-frames` must be new or empty: archive an earlier take before a new take.

After recording has exited, encode:

```
node tools/encode_kuzuha_stage.mjs <ffmpeg.exe> <stage-frames> <stage-c.mp4>
```

Review beginning/middle/end frames and check duration, frame count and video
codec. Recording is production artifact generation, not a claim of real-time
24 fps performance. Preserve capture logs and note unresolved rays and missing
textures. Attach the MP4, generated maps and a UTF-8 explanation to the user's
session. Successful API acceptance alone is not Discord delivery confirmation.
