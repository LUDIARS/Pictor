# Pictor Web mesh module

WebAssembly (C++ `WebMeshRenderer`) + WebGL2 renderer for indexed triangle meshes with
per-vertex position, normal and colour. Shading: hemisphere ambient, up to two directional
lights (Lambert diffuse + GGX specular on one metal/rough surface), sRGB output without tone
mapping. Spec: `spec/feature/web-mesh-module.md` (SPEC-PC-WEB-MESH).

The host owns the canvas, the camera maths and the frame loop. Pictor owns the WebGL2 context,
the shader program and every mesh it created; `releaseMesh` and `dispose` free them.

```js
import { createWebMeshRenderer, hexToRgb } from "@ludiars/pictor-web-mesh";

const renderer = await createWebMeshRenderer(canvas);          // canvas attached to the document
renderer.setLighting({ sky: hexToRgb(0xffffff), ground: hexToRgb(0x000000), hemisphereIntensity: 1, directional: [] });
const mesh = renderer.createMesh({ positions, normals, colors, indices }); // colours sRGB by default
renderer.beginFrame({ width, height, viewProjection, cameraPosition, clearColor });
renderer.submit({ mesh, transform });                          // column-major Float32Array(16)
renderer.endFrame();
```

## Build

Emscripten only. `pictor_web_mesh.mjs` / `.wasm` are produced by the CMake target
`pictor_web_mesh` (`PICTOR_BUILD_WEBGL` with an Emscripten toolchain) and copied, together with
this package's JavaScript, into `<build>/web-mesh/`. Package that directory with `npm pack`;
consumers pin the version and record the Pictor commit they built from.

`npm test` runs the JavaScript wrapper tests against an in-process fake module (no browser).
