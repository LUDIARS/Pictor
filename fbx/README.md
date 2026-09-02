# FBX test assets

The `fbx_viewer` demo looks under `fbx/<model_dir>/` (relative to the build
working directory) for its test rig:

```
fbx/model1/
├── model.fbx              # character mesh + skeleton + embedded clips
├── texture/*.tga          # diffuse textures
└── animation/*.fbx        # additional animation clips (cycled via SPACE/N/P)
```

The character data itself is **not committed to this repository** — FBX
and TGA binaries are `.gitignore`d to keep clones small. They are
distributed as a separate zip archive.

## Obtaining the assets

1. Download the latest `pictor-fbx-testset-*.zip` from the repository's
   release / attachment location (ask the maintainer if you can't find it).
2. Unzip it into this directory so that the layout ends up as shown above
   (`fbx/model1/model.fbx`, `fbx/model1/texture/…`, `fbx/model1/animation/…`).
3. Rebuild — the `pictor_fbx_assets` CMake target copies the directory into
   the build output so the viewer can resolve the default path.

Alternatively, pass a custom path to the viewer:

```
pictor_fbx_viewer <path/to/model_dir> [shader_dir] [options]
```

Options: `--fur` (shell fur on), `--shells N`, `--fur-length X`,
`--fur-density X`, `--no-bones`, `--capture out.bmp [--capture-frame N] [--stay]`,
`--bind [obi|guruguru|kikkou|tasuki]` (rope binding: SDF capsule dent + fur crush),
`--tightness X`, `--bind-anim`, `--no-rope`,
`--no-rope-tail` (no loose end trailing to the camera), `--tears` (toon tears; eyes are
found as the two largest dark blobs in the albedo).

## Texture fallback

When a material's FBX texture path cannot be resolved (typical for
exports that embed an absolute path from the artist's machine), the
viewer uses `texture/default.*` if present. Drop the character's albedo
there when the FBX references are unusable.

## Shell fur test rig

For private or otherwise non-redistributable test art, create a local
`fbx/plush/` rig with `model.fbx`, `texture/default.png`, and optional clips
under `animation/`. These files remain uncommitted under the same ignore rules
as the default test set.

```
pictor_fbx_viewer fbx/plush shaders --fur --no-bones
pictor_fbx_viewer fbx/plush shaders --fur --no-bones --bind --tears # 3 waist loops (obi), crying
pictor_fbx_viewer fbx/plush shaders --fur --no-bones --bind kikkou  # diamond lattice
```

## License

The currently published test set uses **ユニティちゃん (Unity-Chan!)** model
assets under the **Unity-Chan! License Terms 2.0** (© Unity Technologies
Japan / UCL). See `model1/LICENSE.md` for attribution and obligations
carried by anyone redistributing the zip.
