# Pictor Kuzuha geometry demo

Windows x64, AVX2-capable CPU, Vulkan GPU/driver and Microsoft Visual C++ 2022
x64 runtime. Open `pictor_kuzuha_demo.exe` in the supplied package.
Keep `model/` and `shaders/` beside it. The executable accepts an explicit FBX
or model directory and shader directory as its first two positional arguments.

## Controls

| Key | Action |
|---|---|
| F1 / F2 / F3 / F4 | Raymarch quality: target 1 / 0.5 / 0.25 / 0.125 pixel tolerance |
| A | Distance/viewport-dependent auto tolerance / fixed world tolerance |
| [ / ] | Interpolation strength 0..1, hold to repeat |
| I | Animate interpolation continuously (raymarch) |
| S | Original linear surface / full PN interpolation |
| F5 | Polynomial raymarch / sampled raster reference |
| C | Textures / clay / geometric normals / march-step heatmap |
| V | Head / whole body |
| Left / Right | Orbit by 10 degrees |
| Up / Down | Zoom |
| B | Optional imported-skeleton skinning, raster reference only |
| L | Bone overlay |
| Escape | Close |

The viewport shows keys, FPS/frame time, alpha, LOD and unresolved patch-rays.
Raymarch is the default: the GPU evaluates the continuous PN polynomial for hits.
Magenta denotes an unresolved ray; zero means no budget failure was counted in the
preceding frame. Counters include occluded patch-rays, not unique visible pixels.
Auto LOD changes numerical detail with camera distance and window size.
It does not reduce the source patch count. The fixed mode uses a world tolerance;
the pixel value labels the quality tier, not a maintained fixed-mode pixel error.
Raster reference F1-F4 use factors 1/2/4/8 (up to 8 million output vertices).
Raymarch and animated interpolation use the bind pose.

## Build and package

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DPICTOR_BUILD_TESTS=OFF
cmake --build build --config Release --target pictor_kuzuha_demo --parallel 4
```

Use `tools/package_kuzuha_demo.ps1` with explicit `-Binary`, `-ShaderDirectory`,
`-Model`, `-TextureDirectory`, `-Destination` paths. It copies the FBX and its
`*_bsc.png` maps locally. No download, source-asset modification or public publishing
occurs. Distribution of source assets remains subject to their existing terms.

## Captures through Excubitor

The catalog has two opt-in entries: `pc-kuzuha-demo` (interactive) and
`pc-kuzuha-capture` (eight comparisons, then exit). Both run the staged
`Pictor/build/kuzuha-demo/pictor_kuzuha_demo.exe` with cwd `Pictor`.
Declare a Concordia testing claim before starting and release after completion.

CLI flags: `--pn-factor 1|2|4|8`, `--pn-strength 0..1`, `--view head|body`,
`--yaw degrees`, `--zoom 0.25..4`, `--display texture|clay|facets|steps`,
`--renderer raymarch|raster`, `--animate-interpolation`, `--fixed-lod`,
`--window-width 240..3840`, `--window-height 240..3840`,
`--capture image.bmp`, `--capture-set directory`, `--pn-report report.jsonl`.
The capture set writes source/PN head images, source/PN geometric-normal images,
and a textured whole-body PN image, alpha 0.5, low LOD, and march-step heatmap.
It waits 32 frames between captures for the displayed timing average to settle.
Reports append renderer, source patch count, surface draws, alpha, quality,
frame time and prior-frame unresolved count. Frame time includes CPU, GPU wait,
presentation and surrounding machine load; it is not a standalone GPU benchmark.

Research: *Curved PN Triangles*, Vlachos et al., I3D 2001,
https://doi.org/10.1145/364338.364387 . See RESEARCH.md in the package for equations,
boundary protection, normal interpolation and limitations. RAYMARCH.md explains
the distance lower bounds, Hart's sphere tracing research and numerical limits.
