# Pictor browser mesh backend

Browser-native JavaScript backend in Pictor, independent of the C++/Emscripten reference sphere demo. No runtime dependencies. The consumer supplies geometry and matrices; Pictor owns WebGL2 resources, shader compilation and draw submission. No WebGL handles cross the public API.

`new PictorRenderer(canvas)` → `createMesh(Float32Array)` → `beginFrame({width,height,viewProjection,clearColor})` → `submit({mesh,transform})` → `endFrame()`. Release each mesh with `releaseMesh` and the renderer with `dispose`.

Geometry uses triangle-list xyz/rgb interleaved float vertices. Matrices are column-major Float32Array values. Width/height are physical pixels. Colors are linear RGB. The backend uses depth testing and two-sided directional lighting. Animation, camera control, mesh LOD and scene streaming belong to consumers. Thumbnail capture must run synchronously after endFrame, before the browser clears its drawing buffer.

Package with `npm pack` from this directory. Consumers should pin the version and artifact integrity; they must not resolve a sibling source checkout at runtime.
