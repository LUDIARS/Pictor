# FBX viewer: external track playback and raw frame output

Spec ID: `SPEC-PC-FBX-TRACK-PLAYBACK`. Scope: `demo/fbx_viewer` only (no change to shared engine modules
except where noted). Game- and application-agnostic: the viewer plays numeric tracks it is given; it does not
know what produced them.

## Purpose

Let a host program drive a skinned FBX character frame by frame (bone rotations and blendshape weights)
and receive the rendered frames as a raw video stream, so that long deterministic clips (minutes, tens of
thousands of frames) can be encoded by an external encoder without writing image files.

## Command line

| Option | Meaning |
|---|---|
| `--track <file.csv>` | Enables track playback. Frame `i` of the CSV drives rendered frame `i`. Clips are not played. Rendering time advances by `1 / fps` per frame, independent of wall clock. |
| `--track-fps <n>` | Frames per second of the track (1..120, default 30). Used for `elapsed`-driven effects only. |
| `--raw-out <path or ->` | Writes every rendered frame as raw BGRA8 (`width * height * 4` bytes, top row first, no header) to the file or to stdout (`-`). The process exits after the last track frame has been written. Logs must go to stderr while `-` is used. |
| `--size <W>x<H>` | Window / swapchain size (default unchanged). |
| `--camera <ex,ey,ez,tx,ty,tz>` | Fixed camera eye and target in model space (metres), overriding the orbit camera. |
| `--fov <deg>` | Vertical field of view (default unchanged). |
| `--clear <r,g,b>` | Clear colour, components 0..1. |
| `--list-channels` | Prints every bone name (with parent and bind-pose head position in model space) and every blendshape name of the loaded model as JSON on stdout, then exits without opening a window. Lets a host build its name mapping. |

`--raw-out` without `--track` is an error. Existing options (`--capture`, `--record-*`, fur, rope, tears …)
keep their behaviour; combining `--raw-out` with `--record-*` is an error.

## Track file (CSV, UTF-8)

- First row is the header. Column 0 is `frame`. Every other column is one channel:
  - `bone:<BoneName>.rx`, `.ry`, `.rz` — rotation in degrees, applied **on top of the bone's bind-pose local
    rotation** (local space, intrinsic X then Y then Z). Missing axes are 0.
  - `morph:<ShapeName>` — blendshape weight 0..1 (FBX `DeformPercent` 100 = weight 1).
- Each following row is one frame; `frame` must be 0,1,2… without gaps. Values are decimal numbers.
- Unknown bone / shape names: warn once on stderr and ignore the column (so one track can target models with
  slightly different rigs). Zero known channels is an error.
- The whole file is parsed once at start-up into flat per-channel arrays (DoD: no per-frame allocation).

## Behaviour per frame

1. Bones: for each bone channel group, `local = bind_local * R(rx, ry, rz)` and apply with
   `AnimationSystem::set_fk_override(handle, bone_index, local, 1.0f)`, then `update()`.
2. Blendshapes: `position = base_position + Σ weight_k * delta_k` for every morph channel with weight > 0,
   written into the existing host-visible vertex buffer before skinning. Normals may stay unchanged.
   Precompute per-shape sparse delta lists at load time (vertex index + delta) from
   `SkinMeshDescriptor::morph_deltas` / `morph_target_names`; vertices are re-based from the original
   positions every frame (no accumulation drift).
3. Render as today, then, when `--raw-out` is set, read back the swapchain image (reuse the `FrameCapture`
   read-back path, generalised to "every frame into a reusable staging buffer") and write BGRA rows.

## Verification (machine checkable)

- Headless unit test (CTest) for the CSV parser: header mapping, degree→quaternion order, gap / non-numeric
  rejection, unknown-name tolerance.
- Headless unit test for morph application: base + 0.5 × delta for a two-vertex fixture, idempotent across
  frames.
- Build passes with MSVC and GCC (`demo` targets).
- Manual: `pictor_fbx_viewer fbx/model1 --track t.csv --raw-out - --size 640x720 | ffmpeg -f rawvideo
  -pix_fmt bgra -s 640x720 -r 30 -i - out.mp4` produces a clip whose mouth / eyelids follow the morph columns.

## Implementation notes

Task record: `spec/tasks/2026-10-08-fbx-track-playback.md` (Actio `actio:9ca6eef3-daf6-4e8a-98d9-55568d467c4b`).

- Camera coordinates are in the model's own units. `fbx/model1` is authored in centimetres
  (head bone at y ≈ 130), so `--camera` values for it are centimetres as well.
- Blendshapes are not part of the viewer cache; `--track` and `--list-channels` always import the FBX.
- `--track` without `--raw-out` loops the track in the window (preview); with `--raw-out` it plays once.
- The bone overlay stays on by default as before; pass `--no-bones` for a clean raw stream.
- `--track` / `--list-channels` cannot be combined with PN reconstruction (it rebuilds the mesh).
