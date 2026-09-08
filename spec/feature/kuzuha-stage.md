# SPEC-PC-KUZUHA-STAGE — PN stage presentation

`--stage` opts the polynomial demo into a dark stage presentation. Default viewer
lighting and HUD remain available without this flag. The shader shades the actual
PN hit, with independent preset roughness for source Face, Hair, Crown and cloth
groups. Gold uses a metallic GGX lobe; red crown gem regions stay dielectric.
These are art-directed presets, not measured material properties or texture maps.
Hair uses an isotropic lobe in this iteration, not a strand scattering model.

A three-sample vertical light approximation moves across the model with weak
blue fill and magenta rear light. It does not trace shadows or simulate an area
emitter exactly. Stage time is carried in the existing SceneUBO lightDir.w;
negative lightColor.w explicitly selects stage shading. Other viewers do not
interpret these stage-only values. Capture time is frame index / recording fps,
so rendering stalls do not change the light or camera progression.

The stage camera performs a small orbit and push-in around the head. It is a
camera presentation, not full skeletal animation.
The optional `--stage-orbit-curve` accepts increasing `time value` text keys,
loaded once. A one-second normalized KS head-rotation reference is stretched to
18 seconds and drives only the small camera orbit. No KS source dependency is
introduced into Pictor. Without a curve the presentation uses a sinusoidal orbit.
Generated face and crown atlases are local production assets derived from the user's source maps. They
are not committed with source code. Geometry remains the imported PN model.

Operational recording uses a dedicated package in Pictor/build/kuzuha-stage-demo
and Excubitor from the Pictor main folder. Source compilation stays in a task
worktree. No existing hair physics settings or capture directories are replaced.
Texture maps currently have one mip level; this prototype does not claim a
general antialiasing solution for newly generated detail.
