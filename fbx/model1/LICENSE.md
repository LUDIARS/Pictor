# Test Asset — ユニティちゃん (Unity-Chan!) model

The FBX model, textures, and animation clips under `fbx/model1/` are
character assets of **"ユニティちゃん" (Unity-Chan!)** owned by
**© Unity Technologies Japan / UCL**, redistributed here solely as a
test asset for the Pictor FBX viewer demo.

## License

Provided under the **Unity-Chan! License Terms (UCL) 2.0**.

- Project page: <https://unity-chan.com/>
- License terms (EN): <https://unity-chan.com/contents/guideline_en/>
- License terms (JA): <https://unity-chan.com/contents/guideline/>

Key obligations carried by anyone redistributing this directory:

1. Display the Unity-Chan! attribution mark `© Unity Technologies Japan / UCL`
   together with the assets.
2. Do **not** use in content that damages or disgraces the character, nor for
   political / religious / adult / anti-social purposes.
3. Do not sell the assets themselves; use only for permitted creative works
   (including non-commercial and restricted commercial scenarios as defined
   by the UCL).

For authoritative terms, see the URLs above. If you are unsure whether your
downstream use is permitted, refer to the UCL directly instead of relying on
this summary.

## Scope of the included files

- `model.fbx` — character mesh + skeleton + default animation stack.
- `texture/*.tga` — diffuse textures referenced by the model materials
  (only the channels currently consumed by the demo are included; normal /
  specular / environment maps are omitted for repository size).
- `animation/*.fbx` — motion capture / hand-animated clip library used by
  the viewer's keyboard-cycled playback.

These files are **unmodified originals** re-packaged into the directory
layout expected by `pictor_fbx_viewer`.
