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
pictor_fbx_viewer <path/to/model_dir> [shader_dir]
```

## License

The currently published test set uses **ユニティちゃん (Unity-Chan!)** model
assets under the **Unity-Chan! License Terms 2.0** (© Unity Technologies
Japan / UCL). See `model1/LICENSE.md` for attribution and obligations
carried by anyone redistributing the zip.
